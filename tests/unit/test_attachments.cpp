// 附件管线纯逻辑的单测：base64 编解码往返、图片 mime 折叠、上传文件名净化。
// 被测件 = plugins/webui/attachments.hpp（tests 白盒可达的插件根私有头）。
#include "ithaca_test_framework.hpp"

#include "attachments.hpp"

#include <string>

using namespace vc_webui;

VC_TEST("attach: base64 编解码往返——空串/RFC 向量/全字节域") {
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("hello") == "aGVsbG8=");
    CHECK(base64_encode("hello world") == "aGVsbG8gd29ybGQ=");
    CHECK(base64_encode("a") == "YQ==");
    CHECK(base64_encode("ab") == "YWI=");

    // 1000 字节含全字节值（含 0x00 与 >0x7F）：编码 → 解码必须逐字节还原
    std::string raw;
    for (int i = 0; i < 1000; ++i)
        raw.push_back(static_cast<char>(i % 256));
    std::string back;
    REQUIRE(base64_decode(base64_encode(raw), back));
    CHECK(back == raw);

    REQUIRE(base64_decode("aGVsbG8=", back));   // padding 截断收尾
    CHECK(back == "hello");
}

VC_TEST("attach: base64 解码容错——空白忽略、非法字符点名拒绝") {
    std::string out;
    // 浏览器 FileReader 出干净串；容忍换行/空格/制表（宽容口径）
    REQUIRE(base64_decode("aGVs\nbG8=\r\n\t", out));
    CHECK(out == "hello");
    // '=' 截断：其后内容不参与
    REQUIRE(base64_decode("aGVsbG8=GRAG", out));
    CHECK(out == "hello");
    // 非字母表字符（如 '!'、UTF-8 残片）= 失败，不出错字节
    CHECK(!base64_decode("aG!sbG8=", out));
    CHECK(!base64_decode("\xC3\x28", out));
}

VC_TEST("attach: 图片 mime 折叠——大小写不敏感，非图片返回空") {
    CHECK(image_mime_of(".png") == "image/png");
    CHECK(image_mime_of(".PNG") == "image/png");
    CHECK(image_mime_of(".JPG") == "image/jpeg");
    CHECK(image_mime_of(".jpeg") == "image/jpeg");
    CHECK(image_mime_of(".WebP") == "image/webp");
    CHECK(image_mime_of(".gif") == "image/gif");
    CHECK(image_mime_of(".txt").empty());
    CHECK(image_mime_of(".docx").empty());   // 文档类不走多模态
    CHECK(image_mime_of("").empty());
    CHECK(image_mime_of(".pngx").empty());   // 前缀相似不误判
}

VC_TEST("attach: 上传文件名净化——穿越取组件、非法字符剥离、尾点空格剥除") {
    // 路径穿越：只留文件名组件
    CHECK(sanitize_upload_name("..\\evil.png") == "evil.png");
    CHECK(sanitize_upload_name("/etc/passwd") == "passwd");
    CHECK(sanitize_upload_name("a/b/c.png") == "c.png");
    // Windows 非法字符与控制符剥离
    CHECK(sanitize_upload_name("a<b>:c\"d|e?f*.png") == "abcdef.png");
    CHECK(sanitize_upload_name(std::string("x\x01\x1F.png")) == "x.png");
    // 尾部点/空格剥除（Windows 落盘会吞掉——展示名与落盘名分叉的来源）
    CHECK(sanitize_upload_name("报告.png...") == "报告.png");
    CHECK(sanitize_upload_name("报告.png  ") == "报告.png");
    CHECK(sanitize_upload_name(" . . ") == "");
    // 正常名原样
    CHECK(sanitize_upload_name("仪表盘 (1).png") == "仪表盘 (1).png");
    // 全剥 ⇒ 空串（端点侧据此拒绝）
    CHECK(sanitize_upload_name("???") == "");
}
