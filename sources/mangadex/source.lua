-- MangaDex (API v5) for Sumiyomi. API-based: no HTML scraping, no Cloudflare (design doc §9.4).
-- API reference: https://api.mangadex.org/docs/
local Source = {}

local API     = "https://api.mangadex.org"
local SITE    = "https://mangadex.org"
local COVERS  = "https://uploads.mangadex.org/covers"
local LANG    = "en"
local PAGE    = 20       -- results per browse page
local FEED    = 500      -- chapter feed page size (API maximum)
local RATINGS = "&contentRating[]=safe&contentRating[]=suggestive"

local function get_json(path)
    local res = http.get(API .. path, { headers = { Referer = SITE .. "/" } })
    if res.status ~= 200 then
        error(string.format("MangaDex HTTP %d for %s", res.status, path))
    end
    return json.decode(res.body)
end

local function is_null(v) return v == nil or v == json.null end

-- Localized string map ({en = "...", ja = "..."}): English, else the first available.
local function pick(map)
    if is_null(map) then return "" end
    if not is_null(map[LANG]) then return map[LANG] end
    for _, v in pairs(map) do
        if type(v) == "string" then return v end
    end
    return ""
end

local function relationship(entry, kind)
    for _, rel in ipairs(entry.relationships or {}) do
        if rel.type == kind and not is_null(rel.attributes) then return rel.attributes end
    end
    return nil
end

local function names(entry, kind)
    local out = {}
    for _, rel in ipairs(entry.relationships or {}) do
        if rel.type == kind and not is_null(rel.attributes) and not is_null(rel.attributes.name) then
            out[#out + 1] = rel.attributes.name
        end
    end
    return table.concat(out, ", ")
end

local STATUS = { ongoing = 1, completed = 2, hiatus = 6, cancelled = 5 }

local function to_manga(entry)
    local a = entry.attributes
    local m = {
        url   = "/manga/" .. entry.id,
        title = pick(a.title),
    }
    local cover = relationship(entry, "cover_art")
    if cover and not is_null(cover.fileName) then
        m.thumbnail_url = COVERS .. "/" .. entry.id .. "/" .. cover.fileName .. ".256.jpg"
    end
    return m
end

local function manga_list(query, page)
    local offset = (page - 1) * PAGE
    local data = get_json("/manga?limit=" .. PAGE .. "&offset=" .. offset .. "&includes[]=cover_art" .. RATINGS ..
                          "&availableTranslatedLanguage[]=" .. LANG .. "&hasAvailableChapters=true" .. query)
    local mangas = {}
    for _, entry in ipairs(data.data) do mangas[#mangas + 1] = to_manga(entry) end
    return { mangas = mangas, has_next_page = offset + #data.data < data.total }
end

function Source.popular_manga(page)
    return manga_list("&order[followedCount]=desc", page)
end

function Source.latest_updates(page)
    return manga_list("&order[latestUploadedChapter]=desc", page)
end

function Source.search_manga(page, query, filters)
    return manga_list("&title=" .. url.encode(query) .. "&order[relevance]=desc", page)
end

function Source.manga_details(manga)
    local id = manga.url:match("^/manga/([%w%-]+)$")
    if not id then error("not a MangaDex manga url: " .. tostring(manga.url)) end
    local entry = get_json("/manga/" .. id .. "?includes[]=cover_art&includes[]=author&includes[]=artist").data
    local a = entry.attributes
    local m = to_manga(entry)
    m.author      = names(entry, "author")
    m.artist      = names(entry, "artist")
    m.description = str.trim(pick(a.description))
    m.status      = STATUS[a.status] or 0
    local genres = {}
    for _, tag in ipairs(a.tags or {}) do genres[#genres + 1] = pick(tag.attributes.name) end
    m.genres = genres
    return m
end

local function chapter_name(a)
    local parts = {}
    if not is_null(a.volume) then parts[#parts + 1] = "Vol." .. a.volume end
    if not is_null(a.chapter) then parts[#parts + 1] = "Ch." .. a.chapter end
    local name = table.concat(parts, " ")
    if not is_null(a.title) and a.title ~= "" then
        name = name == "" and a.title or (name .. " - " .. a.title)
    end
    return name == "" and "Oneshot" or name
end

function Source.chapter_list(manga)
    local id = manga.url:match("^/manga/([%w%-]+)$")
    if not id then error("not a MangaDex manga url: " .. tostring(manga.url)) end
    local chapters, offset = {}, 0
    repeat
        local data = get_json("/manga/" .. id .. "/feed?limit=" .. FEED .. "&offset=" .. offset ..
                              "&translatedLanguage[]=" .. LANG .. "&order[volume]=desc&order[chapter]=desc" ..
                              "&includes[]=scanlation_group" .. RATINGS .. "&contentRating[]=erotica&contentRating[]=pornographic")
        for _, entry in ipairs(data.data) do
            local a = entry.attributes
            -- External (licensed) chapters have no pages hosted on MangaDex: skip them.
            if is_null(a.externalUrl) and (a.pages or 0) > 0 then
                chapters[#chapters + 1] = {
                    url            = "/chapter/" .. entry.id,
                    name           = chapter_name(a),
                    scanlator      = names(entry, "scanlation_group"),
                    chapter_number = tonumber(not is_null(a.chapter) and a.chapter or "") or -1,
                    date_upload    = time.parse("%Y-%m-%dT%H:%M:%S%z", a.publishAt) or 0,
                }
            end
        end
        offset = offset + #data.data
    until #data.data == 0 or offset >= data.total
    return chapters
end

function Source.page_list(chapter)
    local id = chapter.url:match("^/chapter/([%w%-]+)$")
    if not id then error("not a MangaDex chapter url: " .. tostring(chapter.url)) end
    local server = get_json("/at-home/server/" .. id)
    local pages = {}
    for i, file in ipairs(server.chapter.data) do
        pages[#pages + 1] = { index = i, url = server.baseUrl .. "/data/" .. server.chapter.hash .. "/" .. file }
    end
    return pages
end

return Source
