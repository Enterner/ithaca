<script setup>
// 对话流：记录列表 + 流式块。提案卡（逐文件批准）/「本轮修改」汇总卡内联实现，
// diff 着色走 diff.js 的 v-html（内容全部经 esc，与旧版同信任级别）。
import { ref, watch, nextTick, onMounted } from 'vue'
import { S } from '../store'
import { mdHtml, esc } from '../md'
import { diffHtml, diffFiles, stripHeads } from '../diff'
import { decide } from '../store'

const streamEl = ref(null)
function scrollBottom() {
  if (streamEl.value) streamEl.value.scrollTop = streamEl.value.scrollHeight
}
// 常规跟随：记录数/流式文本变化即滚底
watch(() => [S.records.length, S.live && S.live.text, S.live && S.live.reasoning],
  () => nextTick(scrollBottom), { deep: true })
// NOTE: 初始定位必须主动做——本组件在会话模式下才挂载，挂载时记录往往已装载
// 完毕（数量不再变化 ⇒ watch 永不触发 ⇒ 刷新/切会话后停在顶部，踩过）
onMounted(() => nextTick(scrollBottom))
watch(() => S.curSession, () => nextTick(scrollBottom))

function toolHtml(rec) {
  const warn = rec.type === 'tool' && !rec.ok
  const brief = rec.type === 'tool_call' ? (rec.brief || '') : (rec.brief || '')
  return '<b>' + esc(rec.tool) + '</b> ' + esc(brief)
}
function appliedHtml(rec) {
  if (rec.diff) {
    const m = rec.diff.match(/（\+(\d+) −(\d+) 行）/)
    const st = m ? { a: +m[1], r: +m[2] } : null
    const paths = rec.paths || []
    const single = paths.length <= 1
    const label = single ? (paths[0] || (rec.diff.match(/^── (.+)$/m) || ['', ''])[1] || '')
                         : paths.length + ' 个文件'
    const tag = single && rec.diff.includes('@@ -0,0 +') ? ' <span class="ntag">新文件</span>' : ''
    const stats = st ? ' <i class="add">+' + st.a + '</i> <i class="del">−' + st.r + '</i>' : ''
    return '✔ 已更新 <b>' + esc(label) + '</b>' + tag + stats
  }
  return '✔ 已落盘 ' + esc((rec.paths || []).join(', '))
}

// 提案卡：只负责展示（diff + 逐文件勾选）；裁决走输入栏上方的决策坞（ZCode 式）。
// 勾选/裁决标记挂在记录对象上（store render 时初始化），重放与实时共用。
// 状态列：只有决策坞挂着的「最新」提案显示 待你裁决；其余未决的（历史重放/被
// 更新提案接替的）一律 已失效 并弱化。
function multi(rec) { return (rec._files || []).length >= 2 }
function statText(rec) {
  if (rec === S.pending) return '待你裁决'
  if (rec._decided === 0) return '已批准'
  if (rec._decided === 1) return '已拒绝'
  if (rec._decided === 2) return '已免批'
  return '已失效'
}
function superseded(rec) { return rec !== S.pending && rec._decided === null }

async function onStreamClick(e) {   // 代码块复制按钮（v-html 内容的事件委托）
  const b = e.target.closest('.cpy')
  if (b) {
    await navigator.clipboard.writeText(decodeURIComponent(b.dataset.c))
    b.textContent = '已复制'
    setTimeout(() => { b.textContent = '复制' }, 1200)
  }
}
function toggleSum(sum) { sum._open = !sum._open }
</script>

<template>
  <div ref="streamEl" class="stream" @click="onStreamClick">
    <div class="flow">
      <div v-if="!S.records.length && !S.live" class="empty">启动后在这里看到对话流——输入任务开始。</div>

      <template v-for="(rec, i) in S.records" :key="rec.id ?? 'r' + i">
        <template v-if="rec.type === 'user'">
          <div class="who u">你</div>
          <div class="msg u">{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'assistant' && (rec.text || '')">
          <div class="who">Ithaca</div>
          <div class="msg a" v-html="mdHtml(rec.text || '')"></div>
        </template>

        <template v-else-if="rec.type === 'tool_call' || rec.type === 'tool'">
          <div class="toolline" :class="{ warn: rec.type === 'tool' && !rec.ok }"
               v-html="'⚙ ' + toolHtml(rec)"></div>
        </template>

        <template v-else-if="rec.type === 'applied'">
          <div class="toolline" v-html="appliedHtml(rec)"></div>
        </template>

        <!-- 「本轮修改」汇总卡（记录流内伪记录：结算时已落位到自己轮次） -->
        <template v-else-if="rec.type === 'turn_edits'">
          <div class="who">Ithaca · 本轮修改</div>
          <div class="card edits" :class="{ revoked: rec.revoked }">
            <div class="head">
              <b>本轮修改 {{ rec.files.length }} 个文件</b>
              <span class="stat">
                <template v-if="rec.revoked">已撤销</template>
                <template v-else>
                  <i class="add">+{{ rec.files.reduce((s, f) => s + f.add, 0) }}</i>
                  <i class="del">−{{ rec.files.reduce((s, f) => s + f.rem, 0) }}</i>
                </template>
              </span>
            </div>
            <template v-for="f in rec.files" :key="f.path">
              <div class="erow" :class="{ open: f.open }" @click="f.open = !f.open">
                <span class="chev">▶</span><span class="p">{{ f.path }}</span>
                <span v-if="f.isNew" class="ntag">新文件</span>
                <span class="st">
                  {{ f.n > 1 ? f.n + ' 次编辑 · ' : '' }}
                  <i class="add">+{{ f.add }}</i> <i class="del">−{{ f.rem }}</i>
                </span>
              </div>
              <div class="ebody">
                <div class="diff" v-html="diffHtml(f.applies.length === 1 ? f.applies[0].replace(/^── [^\n]*\n/, '') : f.applies.join(''))"></div>
              </div>
            </template>
          </div>
        </template>

        <template v-else-if="rec.type === 'undone'">
          <div class="toolline">↩ 已撤销（剩 {{ rec.depth }} 组快照）</div>
        </template>

        <template v-else-if="rec.type === 'run_line'">
          <div class="msg">{{ rec.text }}</div>
        </template>
        <template v-else-if="rec.type === 'run_exit'">
          <div class="toolline">⚙ run #{{ rec.run_id }} 退出（exit {{ rec.exit_code }}）</div>
        </template>

        <template v-else-if="rec.type === 'notice'">
          <div v-if="(rec.level || 0) >= 3" class="errcard">{{ rec.text }}</div>
          <!-- 系统通告（sys=1）居中分隔；「模型已切换」「权限模式 = 」通告隐藏
              （选择器自身即反馈，写流属刷屏噪音——踩过：反复切换连发刷屏） -->
          <div v-else-if="rec.sys" class="sysline">── {{ rec.text }} ──</div>
          <div v-else-if="/^(模型已切换|权限模式 = |宿主停止|没有进行中的 turn)/.test(rec.text || '')" style="display:none"></div>
          <div v-else class="msg">{{ rec.text }}</div>
        </template>
        <template v-else-if="rec.type === 'error'">
          <div class="errcard" :title="'错误码 ' + rec.code">⚠ {{ rec.msg }}</div>
        </template>
        <template v-else-if="rec.type === 'local'">
          <div class="msg">{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'proposal'">
          <div class="who">Ithaca · 提案</div>
          <div class="card" :class="{ superseded: superseded(rec) }">
            <div class="head"><b>{{ rec.tool || 'workspace.apply' }}</b><span class="stat">{{ statText(rec) }}</span></div>
            <div v-if="multi(rec)" class="diff">
              <label v-for="p in rec._files" :key="p" class="fhead-row">
                <input type="checkbox" v-model="rec._checked[p]"> {{ p }}
              </label>
            </div>
            <div class="diff" v-html="diffHtml(multi(rec) ? stripHeads(rec.diff) : rec.diff)"></div>
          </div>
        </template>
      </template>

      <!-- 流式块：深度思考（可折叠）+ 正文 + 光标 -->
      <template v-if="S.live">
        <div v-if="S.live.reasoning" class="think" :class="{ closed: !S.live.open }">
          <div class="think-head" @click="S.live.open = !S.live.open">💭 深度思考 ▾</div>
          <div v-if="S.live.open" class="think-body">{{ S.live.reasoning }}</div>
        </div>
        <div v-if="S.live.text || !S.live.reasoning" class="who">Ithaca</div>
        <div v-if="S.live.text || !S.live.reasoning" class="msg a">{{ S.live.text }}<span class="cursor"></span></div>
      </template>

      <!-- 「本轮修改」汇总卡：每轮在自己的位置如实汇报该轮的修改 -->
      <template v-for="(sum, si) in S.editSummaries" :key="'s' + si">
        <div class="who">Ithaca · 本轮修改</div>
        <div class="card edits" :class="{ revoked: sum.revoked }">
          <div class="head">
            <b>本轮修改 {{ sum.files.length }} 个文件</b>
            <span class="stat">
              <template v-if="sum.revoked">已撤销</template>
              <template v-else>
                <i class="add">+{{ sum.files.reduce((s, f) => s + f.add, 0) }}</i>
                <i class="del">−{{ sum.files.reduce((s, f) => s + f.rem, 0) }}</i>
              </template>
            </span>
          </div>
          <template v-for="f in sum.files" :key="f.path">
            <div class="erow" :class="{ open: f.open }" @click="f.open = !f.open">
              <span class="chev">▶</span><span class="p">{{ f.path }}</span>
              <span v-if="f.isNew" class="ntag">新文件</span>
              <span class="st">
                {{ f.n > 1 ? f.n + ' 次编辑 · ' : '' }}
                <i class="add">+{{ f.add }}</i> <i class="del">−{{ f.rem }}</i>
              </span>
            </div>
            <div class="ebody">
              <div class="diff" v-html="diffHtml(f.applies.length === 1 ? f.applies[0].replace(/^── [^\n]*\n/, '') : f.applies.join(''))"></div>
            </div>
          </template>
        </div>
      </template>
    </div>
  </div>
</template>
