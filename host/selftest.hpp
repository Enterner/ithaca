#pragma once
// --selftest 的宿主侧脚手架（main 的唯一 selftest 知识面）：观测订阅、mock 剧本
// 落盘与 env 指路、M1 workspace 冒烟 / M2 直驱 / M3·M4·M5 场景点火、三阶段等待
// 推进与最终裁决。交互模式零参与——渲染归界面插件，宿主只校验管道通不通。
#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "vc_tool.hpp"

#include <nostos/nostos.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ithaca {

class SelftestHarness {
public:
    // 订阅观测端 + 剧本落盘 + env 指路（host.start() 之前调用）。
    // NOTE: 订阅句柄必须活到裁决时刻——放调用方块内会在块尾析构＝立刻全部退订
    // （踩过：selftest 的 done/proposal 全部收不到，20s 超时）。
    void install(nostos::Host<>& host, const std::filesystem::path& ws_dir);
    // 装载完成后调用：agent 在场 ⇒ 剧本 8 轮闭环（M3/M4/M5 全开）；仅 provider ⇒ 等 1 个 done
    void set_agent_present(bool v);

    // M2 冒烟（仅无 agent）：直驱 vc.llm.v1 校验链路。agent 在场时让位——直连
    // chat 会抢走 mock 剧本的轮次，闭环由 agent 驱动
    void m2_drive(nostos::Host<>& host);
    // M1 冒烟（workspace 工具表在时）：完整编辑事务（写 → dry_run → replace → 双撤销，
    // 同步调用不参与等待循环）；随后 M3 点火（agent 在场，turn_id 命名空间互不重叠）
    void m1_smoke_and_kick(nostos::Host<>& host);
    // 三阶段等待推进（主循环每拍调用）：Run1（闭环，等 4 done）→ Run2（回灌，
    // 等 8 done）→ Cancel（长流水中途取消，等"已被取消"通告且 done 不再增长）
    void tick(nostos::Host<>& host);
    bool finished() const { return phase_ == Phase::Done; }

    // 裁决 + 结论打印；返回进程退出码（0 通过 / 3 未通过；宿主关停后调用）
    int verdict();

private:
    enum class Phase { Run1, Run2, Cancel, Done };

    nostos::Host<>* host_ = nullptr;
    std::filesystem::path ws_dir_;
    std::vector<nostos::Subscription> subs_;   // NOTE: 必须活到裁决时刻（见 install）

    bool agent_present_ = false;
    // 剧本完成计数的两阶段真相：Run1 收口 = 4（4 轮编辑 + 3 轮回灌的闭环前段），
    // Run2 收口 = 8（+ 收尾轮）；仅 provider 时等 1 个
    static constexpr int kDonesRun1 = 4;
    static constexpr int kDonesRun2 = 8;
    int expect_dones_ = 1;
    int delta_count_ = 0;
    int done_count_ = 0;
    int error_count_ = 0;
    int proposals_ = 0;
    int delta_at_done8_ = -1;  // done=8 时刻的 delta 快照——取消阶段（turn 9 长内容）
                               // 流出的行数是"取消 vs 回放"的竞态，不计入确定性断言
    bool cancel_seen_ = false;  // M4：取消场景的完成信号（agent 的"已被取消"通告）
    std::vector<std::pair<int, std::string>> run_lines_;  // M5 观测：run 事件归流
    std::vector<std::pair<int, int>> run_exits_;
    bool m4_ok_ = false;   // M4：取消场景结论（无 agent 时视为通过）
    bool m5_ok_ = false;   // M5：构建回灌结论（无 agent 时视为通过）
    bool ws_ran_ = false;  // M1：workspace 冒烟是否运行（工具表在才运行）
    bool ws_ok_ = true;    // 冒烟结论，并入 selftest 退出码
    std::chrono::steady_clock::time_point selftest_t0_{};
    std::chrono::steady_clock::time_point cancel_t0_{};
    bool cancel_sent_ = false;
    Phase phase_ = Phase::Run1;
};

}  // namespace ithaca
