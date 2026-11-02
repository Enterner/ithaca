#pragma once
// 统一工具表的机制件（产品级共享头，准入 = ≥2 工具表插件消费；现状 4 家：
// workspace/office/runner/mcp）。只托管无争议的机制样板：出参缓冲的「调用期
// 有效」契约与发布三连。工具语义（list 自述 JSON / execute 分派）与 ToolRegister
// 通告的取舍留各插件——基类继承刻意不做：C ABI 插件生态的惯例是样板收薄 helper、
// 行为留插件（对齐 vckit::active_gate 的组合式先例）。
//
// 出参契约（vc_tool.hpp 布局纪律）：list/execute 的出参 JSON 指针只在调用期
// 有效——消费方（agent/webui）同步拷贝；实现方把结果写进自己持有的存活缓冲。

#include "vc_events.hpp"  // vc_api::ToolRegister/tool_register：热接入提示事件
#include "vc_tool.hpp"

#include <nostos/abi/plugin.hpp>

#include "nlohmann/json.hpp"

#include <string>

namespace vcplugin {

using json = nlohmann::json;

// 工具级结果出参：序列化进 buf（调用方持有），out 指向其内部——指针仅调用期
// 有效，消费方同步拷贝。工具级失败 = ok:false JSON 经同一路径回灌模型（不是
// 基础设施错误，NOSTOS_OK 只代表"调用成功"）。
inline nostos_status finish_result(std::string& buf, const json& result, const char** out) {
    buf = result.dump();
    *out = buf.c_str();
    return NOSTOS_OK;
}

// 同上，结果已是序列化形态（如 mcp execute 返回 dump 串）——直接落缓冲。
// NOTE: 与 json 重载的分野必须分明：传 std::string 会隐式构造成 JSON 字符串值，
// dump 出的是包着引号的串而非对象（e2e 实测命中）
inline nostos_status finish_result(std::string& buf, std::string result, const char** out) {
    buf = std::move(result);
    *out = buf.c_str();
    return NOSTOS_OK;
}

// 发布三连单源：表装配（thunk 落位）→ publish → 失败日志点名（同名服务已存在
// 是唯一现实成因）。失败返回 NOSTOS_ERR_FAILED——插件激活失败显式可见，消费方
// 对表缺席自带跳过通告，带病继续（表不在却报就绪）没有消费方受益。
inline nostos_status publish_tool_table(
    const nostos::abi::Host& host, vc_api::ToolTable& t, void* self, std::uint64_t svc,
    const char* svc_name, decltype(vc_api::ToolTable::list) list,
    decltype(vc_api::ToolTable::execute) execute, const char* log_tag) {
    nostos::abi::init_table(t);
    t.self = self;
    t.list = list;
    t.execute = execute;
    if (host.publish(svc, &t) != NOSTOS_OK) {
        host.log(4, std::string(log_tag) + ": 发布 " + svc_name + " 失败（同名服务已存在？）");
        return NOSTOS_ERR_FAILED;
    }
    return NOSTOS_OK;
}

// 热接入提示（发现的主通道是 agent 的配置清单/每 turn 取表；这是给面板与 webui
// 看的后到接入信号）。缺席不致命——workspace/office/runner 发，mcp 无消费方不发。
inline void notify_tool_register(nostos::abi::Host& host, const char* svc_name) {
    const vc_api::ToolRegister note{svc_name};
    host.emit(vc_api::tool_register, &note);
}

}  // namespace vcplugin
