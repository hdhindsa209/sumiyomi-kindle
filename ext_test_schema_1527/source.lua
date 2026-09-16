
        local S = {}
        function S.manga_details(m) return m end
        function S.chapter_list(m) return { { url = "/c/1", name = 42 } } end
        function S.page_list(c) return "nope" end
        function S.popular_manga(page)
            return { mangas = { { url = "/m/1", title = "ok" }, { url = "/m/2" } }, has_next_page = false }
        end
        return S