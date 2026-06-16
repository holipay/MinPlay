#include "minunit.h"
#include "../src/network/hls_stream.h"

struct HlsManagerTest {
    static std::wstring ResolveUrl(HlsManager& m, const std::wstring& base, const std::wstring& rel) {
        return m.ResolveUrl(base, rel);
    }
    static bool ParseMediaPlaylist(HlsManager& m, const std::string& content, const std::wstring& base_url) {
        return m.ParseMediaPlaylist(content, base_url);
    }
    static const std::vector<HlsSegment>& Segments(HlsManager& m) { return m.segments_; }
    static double Duration(HlsManager& m) { return m.duration_.load(); }
    static bool IsLive(HlsManager& m) { return m.is_live_.load(); }
};

static void test_resolve_url_simple() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/playlist.m3u8", L"segment1.ts");
    MU_CHECK(r == L"http://example.com/hls/segment1.ts");
}

static void test_resolve_url_absolute_path() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/playlist.m3u8", L"/segments/segment1.ts");
    MU_CHECK(r == L"http://example.com/segments/segment1.ts");
}

static void test_resolve_url_absolute_url() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/playlist.m3u8", L"http://cdn.example.com/seg/1.ts");
    MU_CHECK(r == L"http://cdn.example.com/seg/1.ts");
}

static void test_resolve_url_dotdot() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/v1/playlist.m3u8", L"../segments/segment1.ts");
    MU_CHECK(r == L"http://example.com/hls/segments/segment1.ts");
}

static void test_resolve_url_dot() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/playlist.m3u8", L"./segment1.ts");
    MU_CHECK(r == L"http://example.com/hls/segment1.ts");
}

static void test_resolve_url_query_string_base() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/hls/playlist.m3u8?token=abc", L"segment1.ts");
    MU_CHECK(r == L"http://example.com/hls/segment1.ts");
}

static void test_resolve_url_multiple_dotdot() {
    HlsManager m;
    std::wstring r = HlsManagerTest::ResolveUrl(m,
        L"http://example.com/a/b/c/playlist.m3u8", L"../../x/segment1.ts");
    MU_CHECK(r == L"http://example.com/a/x/segment1.ts");
}

static void test_parse_media_playlist_vod() {
    HlsManager m;
    std::string playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:10\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:10.0,\n"
        "segment0.ts\n"
        "#EXTINF:10.0,\n"
        "segment1.ts\n"
        "#EXT-X-ENDLIST\n";
    bool ok = HlsManagerTest::ParseMediaPlaylist(m, playlist, L"http://example.com/hls/playlist.m3u8");
    MU_CHECK(ok);
    MU_CHECK_EQ(HlsManagerTest::Segments(m).size(), (size_t)2);
    MU_CHECK_DBL(HlsManagerTest::Duration(m), 20.0, 0.001);
    MU_CHECK(!HlsManagerTest::IsLive(m));
}

static void test_parse_media_playlist_live() {
    HlsManager m;
    std::string playlist =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:10\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:10.0,\n"
        "segment100.ts\n"
        "#EXTINF:10.0,\n"
        "segment101.ts\n";
    bool ok = HlsManagerTest::ParseMediaPlaylist(m, playlist, L"http://example.com/live/stream.m3u8");
    MU_CHECK(ok);
    MU_CHECK_EQ(HlsManagerTest::Segments(m).size(), (size_t)2);
    MU_CHECK(HlsManagerTest::IsLive(m));
}

static void test_parse_media_playlist_empty() {
    HlsManager m;
    bool ok = HlsManagerTest::ParseMediaPlaylist(m, "", L"http://example.com/empty.m3u8");
    MU_CHECK(!ok);
}

static void test_parse_media_playlist_with_comma_duration() {
    HlsManager m;
    std::string playlist =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:10\n"
        "#EXTINF:10.000,title with spaces\n"
        "seg0.ts\n"
        "#EXT-X-ENDLIST\n";
    bool ok = HlsManagerTest::ParseMediaPlaylist(m, playlist, L"http://example.com/play.m3u8");
    MU_CHECK(ok);
    MU_CHECK_EQ(HlsManagerTest::Segments(m).size(), (size_t)1);
    MU_CHECK_DBL(HlsManagerTest::Duration(m), 10.0, 0.001);
}

void hls_suite() {
    MU_RUN_TEST(test_resolve_url_simple);
    MU_RUN_TEST(test_resolve_url_absolute_path);
    MU_RUN_TEST(test_resolve_url_absolute_url);
    MU_RUN_TEST(test_resolve_url_dotdot);
    MU_RUN_TEST(test_resolve_url_dot);
    MU_RUN_TEST(test_resolve_url_query_string_base);
    MU_RUN_TEST(test_resolve_url_multiple_dotdot);
    MU_RUN_TEST(test_parse_media_playlist_vod);
    MU_RUN_TEST(test_parse_media_playlist_live);
    MU_RUN_TEST(test_parse_media_playlist_empty);
    MU_RUN_TEST(test_parse_media_playlist_with_comma_duration);
}
