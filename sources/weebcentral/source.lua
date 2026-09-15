-- WeebCentral for Sumiyomi. HTML scraping; selectors follow the Mihon community extension
-- (keiyoushi/extensions-source, src/en/weebcentral). Rate limit 1 request / 2 s (manifest).
local Source = {}

local BASE  = "https://weebcentral.com"
local LIMIT = 32   -- the site always returns 32 per page

local function get_doc(url)
    local res = http.get(url, { headers = { Referer = BASE .. "/" } })
    if res.status ~= 200 then
        error(string.format("WeebCentral HTTP %d for %s", res.status, url))
    end
    return html.parse(res.body)
end

-- "https://weebcentral.com/series/ID/Slug" -> "/series/ID/Slug"
local function relative(href)
    return (href:gsub("^https?://[^/]+", ""))
end

local function list(sort, query, page)
    local url = BASE .. "/search/data?text=" .. url.encode(query) ..
                "&sort=" .. url.encode(sort) .. "&order=Descending&official=Any&anime=Any&adult=Any" ..
                "&display_mode=Full%20Display&limit=" .. LIMIT .. "&offset=" .. ((page - 1) * LIMIT)
    local doc = get_doc(url)
    local mangas = {}
    for _, a in ipairs(doc:select("article > section > a")) do
        local title = a:select_first("div:not([class]):last-child")
        if title then
            mangas[#mangas + 1] = { url = relative(a:attr("href")), title = title:text() }
        end
    end
    return { mangas = mangas, has_next_page = doc:select_first("button") ~= nil }
end

function Source.popular_manga(page) return list("Popularity", "", page) end
function Source.latest_updates(page) return list("Latest Updates", "", page) end

function Source.search_manga(page, query, filters)
    -- The site's search chokes on some punctuation (as the Kotlin extension notes).
    local q = str.trim((query:gsub("[!#:(),%-]", " ")))
    return list("Best Match", q, page)
end

-- The <li> in `section` whose <strong> label contains `label`.
local function field(section, label)
    for _, li in ipairs(section:select("ul > li")) do
        local strong = li:select_first("strong")
        if strong and strong:text():find(label, 1, true) then return li end
    end
    return nil
end

local STATUS = { ongoing = 1, complete = 2, hiatus = 6, canceled = 5 }

function Source.manga_details(manga)
    local doc = get_doc(BASE .. manga.url)
    local sections = doc:select("section[x-data] > section")
    if #sections < 2 then error("unexpected series page layout: " .. manga.url) end
    local meta, main = sections[1], sections[2]

    local out = { url = manga.url, title = main:select_first("h1") and main:select_first("h1"):text() or manga.title }
    local authors = {}
    local li = field(meta, "Author")
    if li then for _, a in ipairs(li:select("span > a")) do authors[#authors + 1] = a:text() end end
    out.author = table.concat(authors, ", ")

    local genres = {}
    for _, label in ipairs({ "Type", "Tag" }) do
        li = field(meta, label)
        if li then for _, a in ipairs(li:select("a")) do genres[#genres + 1] = a:text() end end
    end
    out.genres = genres

    li = field(meta, "Status")
    local status = li and li:select_first("a")
    out.status = status and STATUS[status:text():lower()] or 0

    li = field(main, "Description")
    local p = li and li:select_first("p")
    out.description = p and p:text() or ""
    return out
end

local function parse_date(s)
    if not s or s == "" then return 0 end
    return time.parse("%Y-%m-%dT%H:%M:%S.%f%z", s) or time.parse("%Y-%m-%dT%H:%M:%S%z", s) or 0
end

function Source.chapter_list(manga)
    local id = manga.url:match("^/series/([^/]+)")
    if not id then error("not a WeebCentral series url: " .. tostring(manga.url)) end
    local doc = get_doc(BASE .. "/series/" .. id .. "/full-chapter-list")
    local chapters = {}
    for _, a in ipairs(doc:select("div[x-data] > a")) do
        local name_el = a:select_first("span.flex > span")
        if name_el then
            local name = name_el:text()
            local official = false
            for _, img in ipairs(a:select("img")) do
                if img:attr("src"):lower():find("official", 1, true) then official = true end
            end
            local t = a:select_first("time[datetime]")
            chapters[#chapters + 1] = {
                url            = relative(a:attr("href")),
                name           = name,
                scanlator      = official and "Official" or "",
                chapter_number = tonumber(name:match("(%d+%.?%d*)%s*$") or "") or -1,
                date_upload    = parse_date(t and t:attr("datetime")),
            }
        end
    end
    return chapters
end

-- (Jsoup's `~=` is a regex; CSS `~=` is a word match, so select the strip by id.)
function Source.page_list(chapter)
    local doc = get_doc(BASE .. chapter.url .. "/images?is_prev=False&reading_style=long_strip")
    local pages = {}
    for i, img in ipairs(doc:select("section#chapter-images > img")) do
        pages[#pages + 1] = { index = i, url = img:attr("src") }
    end
    return pages
end

return Source
