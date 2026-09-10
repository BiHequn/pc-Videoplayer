#include "service/media-protocol.h"

#include <cassert>
#include <string>

int main() {
    const std::string request =
        R"({"cmd":24,"video_id":7,"offset":"32768","size":32768})";
    const std::string payload = encodeMediaProtocol(request);
    assert(!payload.empty());
    assert(payload.find('\n') == std::string::npos);
    assert(payload ==
        "LUsHCAtyVlNNSVBEWVZTTgkAAEdVZ0BDFgMUQVVGFBt0WlZSWWhOTVsWG0hVEAwSZF5SXRI=");
    assert(decodeMediaProtocol(payload) == request);
    assert(decodeMediaBase64("not base64%%").empty());
    return 0;
}
