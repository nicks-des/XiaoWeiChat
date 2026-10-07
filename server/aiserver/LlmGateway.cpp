/**
 * @file LlmGateway.cpp
 * @brief LLM 网关实现：Mock / OpenAI 兼容双 Provider。
 */
#include "LlmGateway.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <regex>
#include <sstream>
#include <thread>

#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace lingxi {

namespace {

/** Mock 分片大小（字符）：模拟流式输出节奏 */
constexpr size_t kMockChunkChars = 16;

/**
 * @brief 把完整文本切成语义分片回放（OpenAI 非流式 → 流式体验的桥接）。
 */
void replayAsChunks(const std::string& full, const LlmGateway::StreamCallback& onChunk) {
    // UTF-8 感知分片：切点必须落在字符起始字节（ Protobuf string 字段要求合法 UTF-8）
    const auto isUtf8Lead = [&full](size_t pos) {
        return (static_cast<unsigned char>(full[pos]) & 0xC0) != 0x80;  // 非 10xxxxxx 即起始字节
    };
    size_t start = 0;
    while (start < full.size()) {
        size_t end = std::min(full.size(), start + 48);
        // 标点断句（仅在字符起始字节上判断，避免劈开多字节序列）
        for (size_t i = start + 16; i < end; ++i) {
            if (!isUtf8Lead(i)) {
                continue;
            }
            const unsigned char c = static_cast<unsigned char>(full[i]);
            if (c == ' ' || c == ',' || c == '.') {
                end = i + 1;
                break;
            }
        }
        while (end < full.size() && !isUtf8Lead(end)) {
            ++end;  // 回退到字符边界
        }
        onChunk(full.substr(start, end - start), false, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 打字机节奏
        start = end;
    }
    onChunk("", true, 0);
}

} // namespace

LlmGateway::LlmGateway() = default;

void LlmGateway::loadConfig() {
    auto& config = Config::instance();
    m_provider = config.get<std::string>("llm.provider", "mock");
    m_endpoint = config.get<std::string>("llm.endpoint", "");
    m_model = config.get<std::string>("llm.model", "");
    m_apiKey = config.get<std::string>("llm.apiKey", "");
    m_maxTokens = config.get<int>("llm.maxTokens", 512);
    LX_LOG_INFO("LlmGateway provider={} model={}", m_provider, m_model);
}

std::string LlmGateway::generate(const PromptPack& prompt, const StreamCallback& onChunk) {
    if (m_provider == "openai") {
        return generateOpenAi(prompt, onChunk);
    }
    return generateMock(prompt, onChunk);
}

/* ==================== Mock Provider ==================== */

std::string LlmGateway::generateMock(const PromptPack& prompt, const StreamCallback& onChunk) {
    // 从 system 末段提取触发关键词做确定性回声（测试断言友好）；无则用固定口吻
    std::string userText;
    for (auto it = prompt.messages.rbegin(); it != prompt.messages.rend(); ++it) {
        if (it->role == "user") {
            userText = it->content;
            break;
        }
    }

    std::string reply;
    if (userText.find("蛊") != std::string::npos) {
        reply = "*竹篓里的银蛊轻轻颤了一下* ……你提到蛊，倒是有缘。*她放下药瓶* 南疆的蛊分活死两种，寻常人碰不得。你为何问起？";
    } else if (userText.find("你好") != std::string::npos ||
               userText.find("在吗") != std::string::npos) {
        reply = "*抬眼看了看* 嗯，你好。炉火边坐吧，夜里山路凉。";
    } else {
        reply = "*擦拭着药瓶，头也不抬* ……嗯。这客栈的茶还算热，要不要来一碗？";
    }
    reply += "\n<affinity delta=\"+1\" reason=\"聊得愉快\"/>";

    replayAsChunks(reply, onChunk);
    return reply;
}

/* ==================== OpenAI 兼容 Provider ==================== */

std::string LlmGateway::generateOpenAi(const PromptPack& prompt, const StreamCallback& onChunk) {
    // 请求体（OpenAI /chat/completions 兼容）
    std::ostringstream bodyStream;
    bodyStream << "{\"model\":\"" << m_model << "\",\"max_tokens\":" << m_maxTokens
               << ",\"messages\":[";
    for (size_t i = 0; i < prompt.messages.size(); ++i) {
        const auto& message = prompt.messages[i];
        if (i > 0) {
            bodyStream << ",";
        }
        std::string content = message.content;
        // JSON 字符串转义（引号/反斜杠/换行）
        std::string escaped;
        for (const char c : content) {
            if (c == '"' || c == '\\') {
                escaped += '\\';
            }
            if (c == '\n') {
                escaped += "\\n";
                continue;
            }
            if (c == '\r') {
                continue;
            }
            escaped += c;
        }
        bodyStream << "{\"role\":\"" << message.role << "\",\"content\":\"" << escaped << "\"}";
    }
    bodyStream << "]}";

    // endpoint 解析 http://host:port/path
    std::string url = m_endpoint;
    const bool isHttps = url.rfind("https://", 0) == 0;
    const std::string schemePrefix = isHttps ? "https://" : "http://";
    const std::string rest = url.substr(schemePrefix.size());
    const auto slash = rest.find('/');
    const std::string hostPort = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = hostPort.rfind(':');
    const std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
    const unsigned short port = static_cast<unsigned short>(
        colon == std::string::npos ? (isHttps ? 443 : 80)
                                   : std::stoi(hostPort.substr(colon + 1)));

    try {
        asio::io_context io;
        tcp::socket socket(io);
        asio::ip::tcp::resolver resolver(io);
        asio::connect(socket, resolver.resolve(host, std::to_string(port)));

        http::request<http::string_body> request{http::verb::post, path, 11};
        request.set(http::field::host, host);
        request.set(http::field::content_type, "application/json");
        if (!m_apiKey.empty()) {
            request.set(http::field::authorization, "Bearer " + m_apiKey);
        }
        request.body() = bodyStream.str();
        request.prepare_payload();
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);
        boost::system::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);

        if (response.result() != http::status::ok) {
            LX_LOG_ERROR("openai provider http {}: {}", static_cast<int>(response.result_int()),
                         response.body());
            return "（服务暂时不可用，稍后再试）";
        }
        const auto parsed = nlohmann::json::parse(response.body());
        std::string full = parsed["choices"][0]["message"]["content"].get<std::string>();
        replayAsChunks(full, onChunk);
        return full;
    } catch (const std::exception& e) {
        LX_LOG_ERROR("openai provider exception: {}", e.what());
        return "（服务暂时不可用，稍后再试）";
    }
}

/* ==================== 好感度标记解析 ==================== */

std::string LlmGateway::stripAffinityTag(std::string& text, int& deltaOut) {
    deltaOut = 0;
    // <affinity delta="+N" reason="..."/>（N 可为负）
    std::regex tagRegex(R"rgx(<affinity\s+delta="([+-]?\d+)"[^>]*\/?>)rgx");
    std::smatch match;
    if (std::regex_search(text, match, tagRegex)) {
        try {
            deltaOut = std::clamp(std::stoi(match[1].str()), -3, 3);  // 单次限幅 ±3（docs/04 §8）
        } catch (const std::exception&) {
            deltaOut = 0;
        }
        text = std::regex_replace(text, tagRegex, "");
    }
    // 去除首尾空白
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace lingxi
