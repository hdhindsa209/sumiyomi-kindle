#include "app/reader.h"

#include "core/log.h"
#include "icons.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cstring>

namespace sumi::app {

using namespace sumi::ui;

namespace {

constexpr const char* kEllipsis = "\xE2\x80\xA6";  // "…"
constexpr const char* kDot = " \xC2\xB7 ";         // " · "

// A processed page, centered on white.
// Reading progress along one edge of the page: a white strip so it reads over any artwork, a thin line for the
// whole chapter and a heavy bar for the part already read (from the right in right-to-left reading).
struct ProgressBar {
    int   edge = 0;          // ReaderSettings::progress_bar: 0 off, 1 top, 2 bottom, 3 left, 4 right
    float fraction = 0;      // 0..1 read, counting the page on screen
    bool  rtl = false;
};

class PageView : public Node {
public:
    static constexpr int32_t kBarStrip = 14, kBarHeavy = 8, kBarLine = 2;

    explicit PageView(image::Gray page, ProgressBar bar = {}) : page_(std::move(page)), bar_(bar)
    {
        width = Dim::fill();
        height = Dim::fill();
        opaque = true;
        refresh = Wave::GL16;
    }
    const image::Gray& page() const { return page_; }

protected:
    void paint_content(PaintCtx& ctx) override
    {
        if (page_.px.empty()) return;
        Rect f = frame();
        Rect dst{f.x + (f.w - page_.w) / 2, f.y + (f.h - page_.h) / 2, page_.w, page_.h};
        // Pages taller or wider than the screen (fit width) show their top-left part for now (S7 slices them).
        dst.x = std::max(dst.x, f.x);
        dst.y = std::max(dst.y, f.y);
        Rect c = dst.clipped(ctx.clip);
        if (c.empty()) return;
        const uint8_t* src = page_.px.data() + static_cast<size_t>(c.y - dst.y) * static_cast<size_t>(page_.w)
                           + static_cast<size_t>(c.x - dst.x);
        ctx.canvas.blit_gray8(c, src, page_.w);
    }

    void paint_overlay(PaintCtx& ctx) override
    {
        if (bar_.edge <= 0) return;
        Rect f = frame();
        bool horizontal = bar_.edge <= 2;
        Rect strip = bar_.edge == 1 ? Rect{f.x, f.y, f.w, kBarStrip}
                   : bar_.edge == 2 ? Rect{f.x, f.bottom() - kBarStrip, f.w, kBarStrip}
                   : bar_.edge == 3 ? Rect{f.x, f.y, kBarStrip, f.h}
                                    : Rect{f.right() - kBarStrip, f.y, kBarStrip, f.h};
        ctx.canvas.fill_rect(strip.clipped(ctx.clip), tone::WHITE);
        int32_t length = horizontal ? strip.w : strip.h;
        int32_t done = static_cast<int32_t>(static_cast<float>(length) * std::clamp(bar_.fraction, 0.0f, 1.0f));
        int32_t line_at = (kBarStrip - kBarLine) / 2, heavy_at = (kBarStrip - kBarHeavy) / 2;
        if (horizontal) {
            ctx.canvas.fill_rect(Rect{strip.x, strip.y + line_at, strip.w, kBarLine}.clipped(ctx.clip), tone::BLACK);
            int32_t x = bar_.rtl ? strip.right() - done : strip.x;
            ctx.canvas.fill_rect(Rect{x, strip.y + heavy_at, done, kBarHeavy}.clipped(ctx.clip), tone::BLACK);
        } else {
            ctx.canvas.fill_rect(Rect{strip.x + line_at, strip.y, kBarLine, strip.h}.clipped(ctx.clip), tone::BLACK);
            ctx.canvas.fill_rect(Rect{strip.x + heavy_at, strip.y, kBarHeavy, done}.clipped(ctx.clip), tone::BLACK);
        }
    }

private:
    image::Gray page_;
    ProgressBar bar_;
};

// The tap-zone row doubles as the screen's pager, so hardware page keys reach the reader.
class ZoneRow : public Node {
public:
    explicit ZoneRow(std::function<void(bool)> on_key) : on_key_(std::move(on_key))
    {
        layout = Layout::Row;
        width = Dim::fill();
        height = Dim::fill();
    }
    bool pages() const override { return true; }
    bool on_page(bool forward) override
    {
        on_key_(forward);
        return false;   // the reader presents the new page itself
    }

private:
    std::function<void(bool)> on_key_;
};

std::unique_ptr<Node> zone(int weight, std::function<void()> on_tap)
{
    auto z = std::make_unique<Node>();
    z->width = Dim::fill(weight);
    z->height = Dim::fill();
    z->press_feedback = false;   // a tap on the page mustn't invert a third of it
    z->on_tap = std::move(on_tap);
    return z;
}

std::unique_ptr<Node> stack_fill()
{
    auto n = std::make_unique<Node>();
    n->layout = Layout::Stack;
    n->width = Dim::fill();
    n->height = Dim::fill();
    return n;
}

std::unique_ptr<Node> centered_column()
{
    auto n = std::make_unique<Node>();
    n->layout = Layout::Column;
    n->height = Dim::fill();
    n->opaque = true;
    n->padding = Insets::hv(64, 0);
    n->gap = 40;
    n->align_main = Align::Center;
    n->align_cross = Align::Center;
    return n;
}

Label* big_text(Node* parent, const std::string& text, TypeRole role, FontId font)
{
    auto* l = parent->emplace<Label>(text, role, font, tone::BLACK, 3);
    l->text_align = Align::Center;
    l->width = Dim::fill();
    return l;
}


} // namespace

Reader::Reader(Screen& screen, AppData& data, Callbacks callbacks, Schedule schedule, Frontlight* light, Battery* battery)
    : screen_(screen), data_(data), cb_(std::move(callbacks)), schedule_(std::move(schedule)), light_(light), battery_(battery)
{
}

Reader::~Reader() { detach(); }

void Reader::detach()
{
    if (detached_) return;
    detached_ = true;
    if (load_cancel_) load_cancel_->store(true);           // stop loading the rest of the chapter
    if (finished_ && !view_.pages.empty()) data_.evict_chapter(view_.pages, process_options());   // read to the end: free the cache
    alive_.reset();                                         // pending results are dropped
    screen_.set_pages_per_flash(Screen::kPagesPerFlash);   // lists get their own cadence back
}

void Reader::start(int64_t chapter_id, int start_page, bool from_end)
{
    if (!alive_) return;
    std::weak_ptr<int> alive = alive_;
    loading(std::string("Opening chapter") + kEllipsis);
    data_.reader_settings([this, alive, chapter_id, start_page, from_end](ReaderSettings s) {
        if (alive.expired()) return;
        apply_settings(s);
        data_.open_chapter(chapter_id, [this, alive, chapter_id, start_page, from_end](ChapterView v, std::string err) {
            if (alive.expired()) return;
            if (!err.empty()) {
                present_error("Couldn't open this chapter.\n" + err, [this, chapter_id, start_page, from_end] {
                    start(chapter_id, start_page, from_end);
                });
                return;
            }
            view_ = std::move(v);
            parts_.assign(view_.pages.size(), 0);
            int n = static_cast<int>(view_.pages.size());
            Pos p;
            p.image = from_end ? n - 1 : std::clamp(start_page, 0, n - 1);
            pos_ = p;
            load_chapter("Loading chapter", [this, p, from_end] { show(p, Change::NewScreen, from_end); }, downloaded());
        });
    });
}

bool Reader::downloaded() const
{
    for (const std::string& url : view_.pages)
        if (url.rfind("file://", 0) != 0) return false;
    return !view_.pages.empty();
}

void Reader::load_chapter(const std::string& title, std::function<void()> then, bool background)
{
    if (!alive_ || view_.pages.empty()) return;
    if (load_cancel_) load_cancel_->store(true);
    load_cancel_ = std::make_shared<std::atomic<bool>>(false);
    loaded_ = 0;
    int total = static_cast<int>(view_.pages.size());
    // Reading starts once the next `target` pages (in order, from the current one) are ready; the rest of the
    // chapter keeps loading behind the page. Local files are ready almost at once, so they only wait for one.
    int target = std::min(total, background ? 1 : kReadyPages);
    std::weak_ptr<int> alive = alive_;
    uint64_t req = request_;
    if (!background) {
        req = ++request_;
        waiting_ = true;
        if (loading_shown_ && loading_label_) {   // "Opening chapter" is up: just change its words
            loading_label_->set_text(title + kEllipsis);
            screen_.relayout(loading_label_);
        } else {
            loading(title + kEllipsis);
        }
    }
    auto started = std::make_shared<bool>(false);
    auto start_once = [this, alive, req, started, then, background] {
        if (alive.expired() || *started) return;
        if (!background && req != request_) return;
        *started = true;
        then();   // pages that failed show their own error with a retry when reached
    };
    data_.load_chapter(view_.manga.source_id, view_.pages, pos_.image, process_options(), load_cancel_,
        [this, alive, req, title, target, started, start_once](int loaded, int, int ready) {
            if (alive.expired()) return;
            loaded_ = loaded;
            if (*started) return;
            if (ready >= target) {
                start_once();
                return;
            }
            // Only the count changes, in place: no flash while pages arrive.
            if (req == request_ && loading_shown_ && loading_label_) {
                loading_label_->set_text(title + "\n" + std::to_string(ready) + " of " + std::to_string(target) + " pages");
                screen_.relayout(loading_label_);
            }
        },
        [this, alive, start_once](int loaded, int) {
            if (alive.expired()) return;
            loaded_ = loaded;
            start_once();
        });
}

std::string Reader::subtitle() const
{
    std::string sub = view_.manga.title;
    int n = static_cast<int>(view_.pages.size());
    if (pos_.place == Place::Page) sub += kDot + std::string("Page ") + std::to_string(pos_.image + 1) + " of " + std::to_string(n);
    if (loaded_ < n) sub += kDot + std::string("Loaded ") + std::to_string(loaded_) + " of " + std::to_string(n);   // some pages failed
    return sub;
}

void Reader::apply_settings(const ReaderSettings& s)
{
    settings_ = s;
    screen_.set_pages_per_flash(s.flash_every);
}

image::ProcessOptions Reader::process_options() const
{
    image::ProcessOptions o;
    o.screen_w = screen_.bounds().w;
    o.screen_h = screen_.bounds().h;
    o.fit = settings_.fit;
    o.dither = settings_.dither;
    o.crop_borders = settings_.crop_borders;
    o.split_spreads = settings_.split_spreads;
    o.rtl = rtl();
    static constexpr uint8_t kBlack[] = {0, 12, 30, 50}, kWhite[] = {255, 243, 225, 205};
    static constexpr float kGamma[] = {1.0f, 1.25f, 1.55f, 1.9f};
    static constexpr int32_t kMargin[] = {0, 24, 56, 96};
    o.black_point = kBlack[std::clamp(settings_.contrast, 0, 3)];
    o.white_point = kWhite[std::clamp(settings_.contrast, 0, 3)];
    o.gamma = kGamma[std::clamp(settings_.darkness, 0, 3)];
    o.margin = kMargin[std::clamp(settings_.margin, 0, 3)];
    return o;
}

void Reader::loading(const std::string& text)
{
    std::weak_ptr<int> alive = alive_;
    uint64_t req = request_;
    auto show_page = [this, alive, req, text] {
        if (alive.expired() || req != request_ || (!waiting_ && shown_)) return;
        top_bar_ = bottom_bar_ = nullptr;
        subtitle_ = nullptr;
        menu_open_ = false;
        loading_shown_ = true;
        auto page = loading_page(text, [this] { cb_.exit(); });
        loading_label_ = static_cast<Label*>(page->children()[0].get());
        screen_.set_root(std::move(page), Change::Loading);
    };
    // The very first screen always appears at once; later ones only if the page is slow.
    if (schedule_ && shown_) schedule_(kLoadingDelayMs, show_page);
    else show_page();
}

void Reader::show(Pos pos, Change change, bool want_last_part)
{
    pos_ = pos;
    if (pos.place != Place::Page) {
        present_edge(pos.place);
        return;
    }
    uint64_t req = ++request_;
    waiting_ = true;
    int total = static_cast<int>(view_.pages.size());
    // A settings change re-renders the page in place (menu stays open); anything else may need a loading page.
    // (A loading page already up stays: swapping one loading page for another is a wasted refresh.)
    if (change != Change::Update && !loading_shown_)
        loading("Loading page " + std::to_string(pos.image + 1) + " of " + std::to_string(total) + kEllipsis);
    load(req, pos.image, pos.part, change, want_last_part);
}

void Reader::load(uint64_t req, int image_index, int part, Change change, bool want_last_part)
{
    std::weak_ptr<int> alive = alive_;
    data_.load_page(view_.manga.source_id, view_.pages[static_cast<size_t>(image_index)], part, process_options(),
                    [this, alive, req, image_index, change, want_last_part](PageImage img, std::string err) {
        if (alive.expired() || req != request_) return;
        if (!err.empty()) {
            present_error("Couldn't load page " + std::to_string(image_index + 1) + ".\n" + err, [this] {
                show(pos_, Change::NewScreen);
            });
            return;
        }
        parts_[static_cast<size_t>(image_index)] = img.parts;
        // Going backwards into a split spread: show its last half, not its first (already cached).
        if (want_last_part && img.parts > 1 && pos_.part != img.parts - 1) {
            pos_.part = img.parts - 1;
            load(req, image_index, pos_.part, change, false);
            return;
        }
        pos_.part = std::min(pos_.part, img.parts - 1);
        // A loading page was up: the page replaces it as new content, so it gets the clean flash.
        present_page(img, loading_shown_ && change != Change::Update ? Change::NewScreen : change);
        save_progress();
    });
}

std::unique_ptr<Node> Reader::tap_layer(std::unique_ptr<Node> content)
{
    auto root = stack_fill();
    root->opaque = true;

    // Zones: previous / menu / next, mirrored for right-to-left. They sit *under* the content, so
    // buttons on the start/end pages get their taps and everything else falls through to a zone.
    auto zones = std::make_unique<ZoneRow>([this](bool forward) {
        if (menu_open_) set_menu(false);
        else if (forward) next();
        else prev();
    });
    auto left = [this] { if (menu_open_) set_menu(false); else if (rtl()) next(); else prev(); };
    auto right = [this] { if (menu_open_) set_menu(false); else if (rtl()) prev(); else next(); };
    zones->add(zone(3, left));
    zones->add(zone(4, [this] { toggle_menu(); }));
    zones->add(zone(3, right));
    root->add(std::move(zones));
    root->add(std::move(content));

    root->add(menu_bars());
    return root;
}

std::unique_ptr<Node> Reader::menu_bars()
{
    // Two full-size transparent layers, one aligning its bar to the top, one to the bottom. The bars
    // start hidden; opening the menu lays them out and refreshes only their rects.
    auto layers = stack_fill();

    auto top_layer = stack_fill();
    top_layer->align_cross = Align::Start;   // Stack: align_main is x, align_cross is y
    auto top = std::make_unique<Node>();
    top->layout = Layout::Row;
    top->width = Dim::fill();
    top->height = Dim::px(128);
    top->padding = Insets{8, 0, 32, 0};
    top->gap = 16;
    top->align_cross = Align::Center;
    top->opaque = true;
    top->border.bottom = tone::RULE;
    top->visible = menu_open_;
    auto back = std::make_unique<Node>();
    back->layout = Layout::Stack;
    back->width = Dim::px(96);
    back->height = Dim::px(96);
    back->align_main = Align::Center;
    back->align_cross = Align::Center;
    back->on_tap = [this] { cb_.exit(); };
    back->emplace<Icon>(icon::arrow_back, 24, tone::BLACK);
    top->add(std::move(back));
    auto titles = std::make_unique<Node>();
    titles->layout = Layout::Column;
    titles->width = Dim::fill();
    titles->emplace<Label>(view_.chapter.name, type::LIST_PRIMARY, FontId::InterSemiBold, tone::BLACK)->width = Dim::fill();
    subtitle_ = titles->emplace<Label>(subtitle(), type::LIST_SECONDARY, FontId::InterRegular, tone::BLACK);
    subtitle_->width = Dim::fill();
    top->add(std::move(titles));
    if (battery_ && battery_->percent() >= 0) top->add(std::make_unique<BatteryStatus>(battery_->percent(), battery_->charging()));
    top_bar_ = top_layer->add(std::move(top));
    layers->add(std::move(top_layer));

    auto bottom_layer = stack_fill();
    bottom_layer->align_cross = Align::End;
    bottom_bar_ = bottom_layer->add(bottom_bar());
    layers->add(std::move(bottom_layer));
    return layers;
}

bool Reader::rtl() const
{
    return view_.direction == 1 ? true : view_.direction == 2 ? false : settings_.rtl;
}

std::unique_ptr<Node> Reader::bottom_bar()
{
    // Chapter buttons, then the settings tabs (Reading | Zoom | Crop | Contrast) and the selected
    // tab's options. The options area has a fixed height, so switching tabs replaces the bar in place
    // and refreshes only its rect.
    auto bottom = std::make_unique<Node>();
    bottom->layout = Layout::Column;
    bottom->width = Dim::fill();
    bottom->height = Dim::wrap();
    bottom->padding = Insets{0, 0, 0, 16};
    bottom->opaque = true;
    bottom->border.top = tone::RULE;
    bottom->visible = menu_open_;

    const data::Chapter* older = neighbor(-1);
    const data::Chapter* newer = neighbor(+1);
    auto chapters = std::make_unique<Node>();
    chapters->layout = Layout::Row;
    chapters->padding = Insets{32, 16, 32, 16};
    chapters->gap = 20;
    chapters->height = Dim::px(136);
    if (older) {
        int64_t id = older->id;
        chapters->add(button("Previous chapter", [this, id] { cb_.open_chapter(id, false); }));
    }
    if (newer) {
        int64_t id = newer->id;
        chapters->add(button("Next chapter", [this, id] { cb_.open_chapter(id, false); }));
    }
    if (!older && !newer) chapters->emplace<Label>("Only chapter", type::LIST_SECONDARY, FontId::InterRegular, tone::BLACK);
    bottom->add(std::move(chapters));

    std::vector<std::string> tab_names = {"Reading", "Zoom", "Crop", "Contrast"};
    if (light_) tab_names.push_back("Light");
    bottom->add(tabs(tab_names, tab_, [this](int t) {
        if (t == tab_) return;
        tab_ = t;
        refresh_bottom_bar();
    }));

    auto options = std::make_unique<Node>();
    options->layout = Layout::Column;
    options->height = Dim::px(3 * (44 + 84) + 3 * 8 + 40);   // room for three option rows on every tab
    options->padding = Insets{32, 16, 32, 0};
    options->gap = 8;
    auto row = [&](const std::string& title, std::unique_ptr<Node> control) {
        options->emplace<Label>(title, type::LIST_SECONDARY, FontId::InterSemiBold, tone::BLACK)->width = Dim::fill();
        options->add(std::move(control));
    };
    switch (tab_) {
    case 0: {
        row("This manga", segmented({"Default", "Right to left", "Left to right"}, view_.direction, [this](int i) {
            change([i](ReaderSettings&, int& direction) { direction = i; });
        }));
        static constexpr int kFlash[] = {1, 2, 5, 10, 0};
        int flash_index = 0;
        for (int i = 0; i < 5; ++i)
            if (kFlash[i] == settings_.flash_every) flash_index = i;
        row("Full refresh", segmented({"Every page", "Every 2", "Every 5", "Every 10", "Never"}, flash_index, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.flash_every = kFlash[i]; });
        }));
        row("Progress bar", segmented({"Off", "Top", "Bottom", "Left", "Right"}, settings_.progress_bar, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.progress_bar = i; });
        }));
        break;
    }
    case 1:
        row("Fit", segmented({"Fit page", "Fit width"}, settings_.fit == image::Fit::Width ? 1 : 0, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.fit = i == 1 ? image::Fit::Width : image::Fit::Screen; });
        }));
        row("Double pages", segmented({"Split in two", "Keep whole"}, settings_.split_spreads ? 0 : 1, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.split_spreads = i == 0; });
        }));
        break;
    case 2:
        row("Crop blank borders", segmented({"Auto", "Off"}, settings_.crop_borders ? 0 : 1, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.crop_borders = i == 0; });
        }));
        row("Margins", segmented({"None", "Small", "Medium", "Large"}, settings_.margin, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.margin = i; });
        }));
        break;
    case 4:
        if (light_) {
            options->add(light_control(light_->level(), light_->max(), [this](int level) {
                light_->set(level);
                refresh_bottom_bar();   // only the bar redraws; the page stays
            }));
        }
        break;
    default:
        row("Contrast", segmented({"Low", "Normal", "High", "Max"}, settings_.contrast, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.contrast = i; });
        }));
        row("Darkness", segmented({"Light", "Normal", "Dark", "Darker"}, settings_.darkness, [this](int i) {
            change([i](ReaderSettings& s, int&) { s.darkness = i; });
        }));
        row("Dithering", segmented({"Sharp", "Balanced", "Smooth"}, static_cast<int>(settings_.dither), [this](int i) {
            change([i](ReaderSettings& s, int&) { s.dither = static_cast<image::Dither>(i); });
        }));
        break;
    }
    bottom->add(std::move(options));
    return bottom;
}

void Reader::refresh_bottom_bar()
{
    if (!bottom_bar_ || !menu_open_) return;
    Node* layer = bottom_bar_->parent();
    Rect old = bottom_bar_->frame();
    bottom_bar_ = layer->replace_child(0, bottom_bar());
    screen_.layout_node(layer);
    screen_.repaint(old.united(bottom_bar_->frame()), Wave::GL16);
}

void Reader::change(const std::function<void(ReaderSettings&, int& direction)>& edit)
{
    std::string before = image::PageCache::key("", 0, process_options());
    int settings_before_bar = settings_.progress_bar;
    ReaderSettings s = settings_;
    int direction = view_.direction;
    edit(s, direction);
    bool settings_changed = !(s == settings_);
    if (direction != view_.direction) {
        view_.direction = direction;
        data_.set_manga_direction(view_.manga.id, direction);
    }
    if (settings_changed) {
        apply_settings(s);
        data_.save_reader_settings(s);
    }
    if (image::PageCache::key("", 0, process_options()) == before) {
        // The processed pages are the same. The progress bar is drawn over the page: a change to it shows the
        // page again from the cache under the open menu; anything else only redraws the bar.
        if (settings_.progress_bar != settings_before_bar) show(pos_, Change::Update);
        else refresh_bottom_bar();
        return;
    }
    // The page looks different: re-render it now with the menu still open, so the effect is visible.
    // The rest of the chapter is loaded again once, when the menu closes.
    pages_changed_ = true;
    std::fill(parts_.begin(), parts_.end(), 0);
    pos_.part = 0;
    show(pos_, Change::Update);
}

void Reader::toggle_menu() { set_menu(!menu_open_); }

void Reader::set_menu(bool open)
{
    if (!top_bar_ || !bottom_bar_ || open == menu_open_) return;
    menu_open_ = open;
    if (open) {
        if (subtitle_) subtitle_->set_text(subtitle());
        top_bar_->visible = bottom_bar_->visible = true;
        screen_.layout_node(top_bar_->parent());
        screen_.layout_node(bottom_bar_->parent());
        screen_.repaint(top_bar_->frame(), Wave::GL16);
        screen_.repaint(bottom_bar_->frame(), Wave::GL16);
    } else {
        Rect a = top_bar_->frame(), b = bottom_bar_->frame();
        top_bar_->visible = bottom_bar_->visible = false;
        screen_.repaint(a, Wave::GL16);   // the page underneath comes back
        screen_.repaint(b, Wave::GL16);
        if (pages_changed_) {
            pages_changed_ = false;
            // The page under the menu already shows the new settings: it only comes back if a loading page covered it.
            load_chapter("Applying settings", [this] {
                if (loading_shown_) show(pos_, Change::NewScreen);
                else waiting_ = false;
            }, downloaded());
        }
    }
}

void Reader::present_page(const PageImage& img, Change change)
{
    // tap_layer builds the bars visible if the menu was open (a settings change re-shows the page).
    waiting_ = false;
    loading_shown_ = false;
    ProgressBar bar;
    bar.edge = settings_.progress_bar;
    bar.rtl = rtl();
    int n = std::max<int>(1, static_cast<int>(view_.pages.size()));
    int parts = std::max(1, img.parts);
    bar.fraction = (static_cast<float>(pos_.image) + static_cast<float>(pos_.part + 1) / static_cast<float>(parts)) / static_cast<float>(n);
    screen_.set_root(tap_layer(std::make_unique<PageView>(img.page, bar)), change);
    shown_ = true;
}

void Reader::present_edge(Place place)
{
    ++request_;
    waiting_ = false;
    auto body = centered_column();
    const data::Chapter* older = neighbor(-1);
    const data::Chapter* newer = neighbor(+1);
    if (place == Place::End) {
        big_text(body.get(), "Finished", type::LIST_SECONDARY, FontId::InterMedium);
        big_text(body.get(), view_.chapter.name, type::APP_BAR_TITLE, FontId::InterSemiBold);
        if (newer) {
            int64_t id = newer->id;
            big_text(body.get(), "Next: " + newer->name, type::LIST_PRIMARY, FontId::InterRegular);
            body->add(button("Read next chapter", [this, id] { cb_.open_chapter(id, false); }, true));
        } else {
            big_text(body.get(), "There's no next chapter yet.", type::LIST_PRIMARY, FontId::InterRegular);
        }
    } else {
        big_text(body.get(), "Start of", type::LIST_SECONDARY, FontId::InterMedium);
        big_text(body.get(), view_.chapter.name, type::APP_BAR_TITLE, FontId::InterSemiBold);
        if (older) {
            int64_t id = older->id;
            big_text(body.get(), "Previous: " + older->name, type::LIST_PRIMARY, FontId::InterRegular);
            body->add(button("Read previous chapter", [this, id] { cb_.open_chapter(id, true); }));
        }
    }
    body->add(button("Back to manga", [this] { cb_.exit(); }));
    menu_open_ = false;
    Change change = shown_ && !loading_shown_ ? Change::PageTurn : Change::NewScreen;
    loading_shown_ = false;
    screen_.set_root(tap_layer(std::move(body)), change);
    shown_ = true;
}

void Reader::present_error(const std::string& message, std::function<void()> retry)
{
    ++request_;
    waiting_ = false;
    auto body = centered_column();
    big_text(body.get(), message, type::LIST_PRIMARY, FontId::InterRegular);
    body->add(button("Retry", std::move(retry), true));
    body->add(button("Back to manga", [this] { cb_.exit(); }));
    top_bar_ = bottom_bar_ = nullptr;
    menu_open_ = false;
    loading_shown_ = false;
    screen_.set_root(std::move(body), Change::NewScreen);
    shown_ = true;
}

const data::Chapter* Reader::neighbor(int step) const
{
    // Chapters are in source order, newest first: "next" (newer) is the one before.
    for (size_t i = 0; i < view_.chapters.size(); ++i) {
        if (view_.chapters[i].id != view_.chapter.id) continue;
        long j = static_cast<long>(i) - step;
        if (j < 0 || j >= static_cast<long>(view_.chapters.size())) return nullptr;
        return &view_.chapters[static_cast<size_t>(j)];
    }
    return nullptr;
}

void Reader::next()
{
    if (view_.pages.empty()) return;
    int n = static_cast<int>(view_.pages.size());
    switch (pos_.place) {
    case Place::Start:
        show({Place::Page, 0, 0}, Change::PageTurn);
        return;
    case Place::End:
        if (const data::Chapter* newer = neighbor(+1)) cb_.open_chapter(newer->id, false);
        return;
    case Place::Page:
        break;
    }
    int parts = parts_[static_cast<size_t>(pos_.image)];
    if (pos_.part + 1 < parts) show({Place::Page, pos_.image, pos_.part + 1}, Change::PageTurn);
    else if (pos_.image + 1 < n) show({Place::Page, pos_.image + 1, 0}, Change::PageTurn);
    else show({Place::End, pos_.image, pos_.part}, Change::PageTurn);
}

void Reader::prev()
{
    if (view_.pages.empty()) return;
    int n = static_cast<int>(view_.pages.size());
    switch (pos_.place) {
    case Place::Start:
        return;
    case Place::End:
        show({Place::Page, n - 1, 0}, Change::PageTurn, true);
        return;
    case Place::Page:
        break;
    }
    if (pos_.part > 0) show({Place::Page, pos_.image, pos_.part - 1}, Change::PageTurn);
    else if (pos_.image > 0) show({Place::Page, pos_.image - 1, 0}, Change::PageTurn, true);
    else show({Place::Start, 0, 0}, Change::PageTurn);
}

bool Reader::on_back()
{
    if (menu_open_) {
        set_menu(false);
        return true;
    }
    return false;
}

void Reader::save_progress()
{
    if (view_.chapter.id <= 0 || view_.pages.empty()) return;
    int n = static_cast<int>(view_.pages.size());
    bool last = pos_.image == n - 1 && pos_.part + 1 >= std::max(1, parts_[static_cast<size_t>(pos_.image)]);
    finished_ = finished_ || last;
    data_.save_progress(view_.chapter.id, pos_.image, n, last);
}

} // namespace sumi::app
