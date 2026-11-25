#pragma once

#include <string>

#include "podcast_types.h"

// 一丢读书 Vercel API 客户端。
//
// NVS namespace "podcast":
//   api_base      https://<vercel-host>
//   device_token  设备 Bearer Token（服务端只保存哈希）
//
// 设备端只持有自己的 Token，不接触 Supabase Service Role Key。
class PodcastClient {
public:
    static PodcastList FetchPodcasts(std::string& error);
    static PodcastPlayInfo ResolvePlayUrl(const std::string& podcast_id,
                                          std::string& error);
    // 音频地址返回 403 时，只失效该节目对应的签名地址缓存。
    static void InvalidatePlayUrl(const std::string& podcast_id);
    // 手动刷新：列表接口有 30 分钟内存缓存，必须显式失效才会真正重拉。
    static void InvalidateListCache();
    // 手动刷新时一并丢掉旧的签名播放地址，避免继续用快过期的地址。
    static void InvalidateAllPlayUrls();
    static std::string DeviceToken();
    // 只对自家域名（api_base 的 host）附 Bearer：签名音频地址在第三方域，
    // 不该带着设备令牌过去。
    static bool IsOwnHost(const std::string& url);
};
