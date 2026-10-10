# skills/ —— 本仓 skill 正本目录（单一事实源）

任何 agent 可用的 skill 一律放这里：`skills/<skill名>/SKILL.md`，标准
Agent Skills 格式（frontmatter：name + description；正文为操作指令）。

## 各家 agent 的接线方式

没有哪个 skill 目录能被所有 agent 自动发现，所以新增 skill 后要做两件事：

1. **正文**：写在 `skills/<名>/SKILL.md`（本目录，git 跟踪）。
2. **薄指针**：在每个 agent 专属目录放一个只含指针的 SKILL.md，指回正本：
   - 如ZCode：`.zcode/skills/<名>/SKILL.md`
   - 只读 AGENTS.md 的 agent：AGENTS.md 的「Skills」节已统一指引到本目录。

## 现有 skill

- `write-tests` —— 写/改测试前必读，规范正文在 `docs/测试规范.md`。
