#include "app/reader.h"

#include "core/log.h"
#include "icons.h"
#include "ui/widgets.h"

#include <algorithm>

namespace sumi::app {

using namespace sumi::ui;

namespace {

constexpr const char* kEllipsis = "\xE2\x80\xA6";  // "…"
constexpr const char* kDot = " \xC2\xB7 ";         // " · "

// A processed page, centered on white.
class PageView : public Node {
public:
    explicit PageView(image::Gray page) : page_(std::move(page))
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

private:
    image::Gray page_;
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

Reader::Reader(Screen& screen, AppData& data, Callbacks callbacks, Schedule schedule)
    : screen_(screen), data_(data), cb_(std::move(callbacks)), schedule_(std::move(schedule))
{
}

Reader::~Reader() { detach(); }

void Reader::detach()
{
    if (load_cancel_) load_cancel_->store(true);           // stop loading the rest of the chapter
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
            show(p, Change::NewScreen, from_end);
            load_chapter();
        });
    });
}

void Reader::load_chapter()
{
    if (!alive_ || view_.pages.empty()) return;
    if (load_cancel_) load_cancel_->store(true);
    load_cancel_ = std::make_shared<std::atomic<bool>>(false);
    loaded_ = 0;
    std::weak_ptr<int> alive = alive_;
    data_.load_chapter(view_.manga.source_id, view_.pages, pos_.image, process_options(), load_cancel_,
                       [this, alive](int loaded, int) {
                           if (alive.expired()) return;
                           loaded_ = loaded;
                           if (menu_open_ && subtitle_) subtitle_->set_text(subtitle());   // only the bar refreshes
                       });
}

std::string Reader::subtitle() const
{
    std::string sub = view_.manga.title;
    int n = static_cast<int>(view_.pages.size());
    if (pos_.place == Place::Page) sub += kDot + std::string("Page ") + std::to_string(pos_.image + 1) + " of " + std::to_string(n);
    sub += kDot + (loaded_ >= n ? std::string("Chapter loaded") : "Loaded " + std::to_string(loaded_) + " of " + std::to_string(n));
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
        screen_.set_root(loading_page(text, [this] { cb_.exit(); }), Change::Loading);
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
    if (change != Change::Update)
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
    top_bar_ = top_layer->add(std::move(top));
    layers->add(std::move(top_layer));

    auto bottom_layer = stack_fill();
    bottom_layer->align_cross = Align::End;
    auto bottom = std::make_unique<Node>();
    bottom->layout = Layout::Column;
    bottom->width = Dim::fill();
    bottom->height = Dim::wrap();
    bottom->padding = Insets{32, 24, 32, 24};
    bottom->gap = 20;
    bottom->opaque = true;
    bottom->border.top = tone::RULE;
    bottom->visible = menu_open_;
    auto row1 = std::make_unique<Node>();
    row1->layout = Layout::Row;
    row1->gap = 20;
    const data::Chapter* older = neighbor(-1);
    const data::Chapter* newer = neighbor(+1);
    if (older) {
        int64_t id = older->id;
        row1->add(button("Previous chapter", [this, id] { cb_.open_chapter(id, false); }));
    }
    if (newer) {
        int64_t id = newer->id;
        row1->add(button("Next chapter", [this, id] { cb_.open_chapter(id, false); }));
    }
    if (older || newer) bottom->add(std::move(row1));
    auto row2 = std::make_unique<Node>();
    row2->layout = Layout::Row;
    row2->gap = 20;
    // Direction applies to this manga (manga vs. webtoons differ); the default lives in Settings.
    row2->add(button(rtl() ? "Right to left" : "Left to right", [this] {
        view_.direction = rtl() ? 2 : 1;
        data_.set_manga_direction(view_.manga.id, view_.direction);
        menu_open_ = true;
        pos_.part = 0;   // split-spread order flips with direction
        show(pos_, Change::Update);
        load_chapter();  // pages are processed per direction: load the chapter again for the new one
    }));
    row2->add(button("Settings", [this] { present_settings(); }, false, icon::settings));
    bottom->add(std::move(row2));
    bottom_bar_ = bottom_layer->add(std::move(bottom));
    layers->add(std::move(bottom_layer));
    return layers;
}

bool Reader::rtl() const
{
    return view_.direction == 1 ? true : view_.direction == 2 ? false : settings_.rtl;
}

void Reader::present_settings()
{
    ++request_;
    waiting_ = false;
    loading_shown_ = false;
    in_settings_ = true;
    menu_open_ = false;
    top_bar_ = bottom_bar_ = nullptr;
    subtitle_ = nullptr;
    // What the pages look like before, to know on Done whether the chapter must be processed again.
    if (!settings_dirty_) settings_before_ = process_options();
    settings_dirty_ = true;

    auto root = std::make_unique<Node>();
    root->layout = Layout::Column;
    root->height = Dim::fill();
    root->opaque = true;
    root->add(app_bar("Reader settings", [this] { close_settings(); }, {}));

    auto body = std::make_unique<Node>();
    body->layout = Layout::Column;
    body->height = Dim::fill();
    body->padding = Insets{32, 16, 32, 16};
    body->gap = 12;
    auto section = [&](const std::string& title, std::unique_ptr<Node> control) {
        auto* l = body->emplace<Label>(title, type::LIST_PRIMARY, FontId::InterSemiBold, tone::BLACK);
        l->width = Dim::fill();
        body->add(std::move(control));
        auto spacer = std::make_unique<Node>();
        spacer->height = Dim::px(20);
        body->add(std::move(spacer));
    };
    auto save = [this](ReaderSettings s) {
        apply_settings(s);
        data_.save_reader_settings(s);
        present_settings();
    };

    section("This manga", segmented({"Default", "Right to left", "Left to right"}, view_.direction, [this](int i) {
        view_.direction = i;
        data_.set_manga_direction(view_.manga.id, i);
        present_settings();
    }));
    section("Default reading direction", segmented({"Right to left", "Left to right"}, settings_.rtl ? 0 : 1, [this, save](int i) {
        ReaderSettings s = settings_;
        s.rtl = i == 0;
        save(s);
    }));
    static constexpr int kFlash[] = {1, 2, 5, 10, 0};
    int flash_index = 0;
    for (int i = 0; i < 5; ++i)
        if (kFlash[i] == settings_.flash_every) flash_index = i;
    section("Full refresh (flash)", segmented({"Every page", "Every 2", "Every 5", "Every 10", "Never"}, flash_index, [this, save](int i) {
        ReaderSettings s = settings_;
        s.flash_every = kFlash[i];
        save(s);
    }));
    section("Dithering", segmented({"Sharp", "Balanced", "Smooth"}, static_cast<int>(settings_.dither), [this, save](int i) {
        ReaderSettings s = settings_;
        s.dither = static_cast<image::Dither>(i);
        save(s);
    }));
    section("Crop blank borders", segmented({"On", "Off"}, settings_.crop_borders ? 0 : 1, [this, save](int i) {
        ReaderSettings s = settings_;
        s.crop_borders = i == 0;
        save(s);
    }));
    section("Split double pages", segmented({"On", "Off"}, settings_.split_spreads ? 0 : 1, [this, save](int i) {
        ReaderSettings s = settings_;
        s.split_spreads = i == 0;
        save(s);
    }));
    root->add(std::move(body));
    auto done = std::make_unique<Node>();
    done->padding = Insets{32, 0, 32, 32};
    done->add(button("Done", [this] { close_settings(); }, true));
    root->add(std::move(done));
    // First entry is a new screen; option changes redraw it in place without a flash.
    screen_.set_root(std::move(root), settings_shown_ ? Change::Update : Change::NewScreen);
    settings_shown_ = true;
}

void Reader::close_settings()
{
    if (!in_settings_) return;
    in_settings_ = false;
    settings_shown_ = false;
    settings_dirty_ = false;
    image::ProcessOptions now = process_options();
    bool reprocess = image::PageCache::key("", 0, now) != image::PageCache::key("", 0, settings_before_);
    if (reprocess) pos_.part = 0;
    show(pos_, Change::NewScreen);
    if (reprocess) load_chapter();
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
    }
}

void Reader::present_page(const PageImage& img, Change change)
{
    // tap_layer builds the bars visible if the menu was open (a settings change re-shows the page).
    waiting_ = false;
    loading_shown_ = false;
    screen_.set_root(tap_layer(std::make_unique<PageView>(img.page)), change);
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
    if (in_settings_) {
        close_settings();
        return true;
    }
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
    data_.save_progress(view_.chapter.id, pos_.image, n, last);
}

} // namespace sumi::app
