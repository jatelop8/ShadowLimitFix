# SLF-B：材质阴影消费端字节码补丁 — 设计 v1

> 日期：2026-09-04 | 项目：ShadowLimitFix（ENB 共存 / 零 CS 运行时）
> 状态：设计定稿，待施工。上游结论来自 fix19o-fix20 实测 + 2720 个引擎 PS 全量反汇编 + open-shaders CS LLF 源码对照。

---

## 0. 结论摘要

- **闪的根因（定案）**：引擎材质 PS 阴影 = 查表 t14（屏幕空间 4 通道 mask）。引擎每帧从场景阴影灯池轮换挑 ≤4 盏烘焙 → >4 盏场景帧间跳名单 = "地面一块亮一块黑"。
- **渲影侧（已完成）**：SLF 1/1/1 配置把 24 盏全量渲进 kSHADOWMAPS 扩展 slice（读回实证真内容）。**缺的唯一一环 = 材质消费端**。
- **消费端唯一 >4 盏正解**：给引擎原版材质 PS 打字节码补丁 —— 把 9 指令"查表取遮蔽"单元换成"逐灯实时 shadow test"（读数据通道矩阵 → 投影 → SampleCmp kSHADOWMAPS[shadowMapIndex]）。CS/LLF 同语义（GetShadowLightShadow），但我们是**字节码级**而非替换材质，保留引擎光照 = ENB 兼容、材质不退化。
- **工程量评估（诚实）**：数天级子项目。字节码改写 + 资源绑定 + 渲影相机数据质量 + 全变体验证 = 4 个工作块。不是一夜工程。

---

## 1. 已锁定事实

### 1.1 渲影侧（SLF 现状，全部实证）
- kSHADOWMAPS = 127-slice R16_TYPELESS depth array（DSV D16_UNORM），引擎渲影循环 dispatch 被我们接管（SLF_MANUAL_RENDER=1 + SLF_SKIP_VANILLA_DISPATCH=1）。**引擎自己 dispatch 必崩**（crash-09-03-12-04-19 + crash-09-04-16-41-43，14CC19E/14CC1A2 同一循环 4 字节内）→ 渲影只能我们渲。
- SelectDSB/扩展槽固定（P2_EXTEND，slices 8-29 pinning）+ 读回内容真实（fix19n D16 读回 r0000ffff + 阴影形状）。

### 1.2 材质消费语义（2720 个 .fxcb 反汇编实证）
- **t14 sample**（屏幕 UV，v0→cb12[44]/cb0[2] 变换）：`sample_indexable(texture2d) rM.xyzw, uv, t14, s14`，结果寄存器 M ∈ {r5(424 文件)/r4(177)/r6(94)/r7(32)}。
- **9 指令阴影提取单元**（灯循环内，727 文件全命中，**7 模板 = 同一结构仅寄存器槽位不同**）：

```
lt   rA.w, loopIdx, shadowCount    ; loopIdx < min(cb2[29].y, 4)?
if_nz rA.w
  ftou rA.w, loopIdx
  dp4  rA.w, cb2[2].xyzw, icb[loopIdx]     ; channel = cb2[2][loopIdx]  (引擎 CPU 填)
  ftou rA.w, rA.w
  dp4  rA.w, rM.xyzw, icb[channel]         ; shadow = t14mask[channel]
else
  mov  rA.w, l(1.0)                        ; 无阴影 → 全亮
endif
```

- 模板分布：335/177/82/64/58/10/1（=727）。阴影数硬上限 `min(cb2[29].y, 4)`；照明灯循环上限 `min(cb2[29].x, 7)`。
- **cb2[2].xyzw 语义**：第 i 盏照明灯（循环序）在 t14 的通道号。**CPU 侧填充者未锁定**（考古任务 T1）。
- 引擎 cbuffer **支持动态索引**（灯循环 `cb2[r9.x+15]` 实证）→ 数据通道可仿 cb2 动态索引（但见 §5 风险）。

### 1.3 CS/LLF 权威参考（open-shaders ShadowEngineHooks.cpp）
- 阴影灯池 = `ShadowSceneNode::shadowLightsAccum[]`；活跃标记 = `activeLightMask`（uint32，32bit）+ firstPersonShadowMask。
- per-surface 选灯（CS hook `CalculateActiveLightsForSurface` 100997/107784 语义）：sun 固定 lights[0] → Step1 vanilla mask 内灯 → Step2 扩展池注入（越过 32bit 的灯直接数组扫描 + LightContainsCamera 准入）→ Step3 非阴影灯；`shadowCount` = 该 surface 阴影灯数。
- **LLF 材质消费 = 逐灯 GetShadowLightShadow（直接读每灯 shadow，不经 t14）**——印证补丁形态。
- **utility pass no-op**：原版 `RenderShadowLightsWithUtilityShader`（100423/107141）用 maskIndex 索引 4-entry blend 表，SLF 扩展槽 >4 → OOB → **CS 直接 detour 空函数**（且禁止调 ReturnShadowmaps——会清 shadowmapDescriptors 破坏级联矩阵上传）。SLF 必须同样 no-op（架构文档 Stage A 缺口，**未实现**，施工块 B3）。
- `GetShadowMask`/`GetMaskIndex`/`GetAccumLightSlot` 全局（uid 528093/528091 系）= 引擎调度全局，CS 维护它们。

### 1.4 SLF 数据通道现状（已实现，Scheduler.cpp）
- `ShadowLightData{proj[16], pos[3], radius, shadowMapIndex, flags, pad[3]}` × 128（HookUtil.h:97）
- 每帧由 Scheduler 填充：pos/radius/shadowMapIndex 取自灯 + descriptor[0]；**proj 从 descriptor[0].camera（NiCamera worldToCam + viewFrustum）重建**（ortho/persp 两式）；flags=1 矩阵有效。
- `g_shadowLightCount` 原子发布；每 256 帧 health 日志。
- 健康佐证：fix20 crash run 16:41:43 崩溃前一刻 "4 lights scheduled, 4 published, **4 valid matrices**" → 引擎 accumulator 灯相机全有效。

---

## 2. 补丁形态（决策）

### 2.1 目标
材质 PS 循环内每盏阴影灯 i 的遮蔽从"t14 查表"改为"实时 test"：

```
shadow_i = (投影 v2 到灯 i 阴影空间 → uv.z 在 [0,1] 且 SampleCmp(kSHADOWMAPS, uv, depth, shadowMapIndex_i))
```

### 2.2 补丁点与约束
- **落点**：HookCreatePixelShader（ShaderReplace.cpp:43，vtable[15]，已装）——改写 a_bytecode 后调 g_origCreatePS。
- **模板**：只服务 7 模板（727 文件）共有的 9 指令单元；其它 464 个 t14 文件（非 cb2[2] 模式）与 1529 个无 t14 文件（非阴影材质/太阳阴影等）不动（P0 只覆盖 727）。
- **等长 vs 跳转**：9 条 → ~25-30 条 = 不等长。方案：把 9 条单元的 if/else 体整体替换为"短前导 + 无条件相对跳转"到 shader 尾部追加的补丁块（DXBC 指令流支持相对跳转标号），跳回循环。**指令级 token 改写可行性是 P0 PoC 的唯一目标**。

### 2.3 补丁内联序列需要的运行时输入
1. 灯 i 数据（矩阵/slice）：**新增 StructuredBuffer SRV**（t 槽复用引擎未用高位槽，见 §5）或 cbuffer；每帧从 g_shadowLights 拷贝（只读，无渲影行为变化）。
2. 表面世界坐标：材质 PS 的 v2（`add r9.yzw, -v2.xyzx, cb2[r9.x+15]` 实证 v2 = 世界坐标）。
3. t103/kSHADOWMAPS comparison sampler（引擎已绑 t103？材质 PS dcl 里 t103 存在性 = 施工前核查项 T2）。

---

## 3. CPU 侧考古任务（施工前置）

| # | 任务 | 方法 | 产出 |
|---|------|------|------|
| T1 | cb2[2]/cb2[29] 填充者定位（引擎每 surface cbuffer 更新点） | IDA/CE 断点 or CS 源码对应 hook 点反查 | 决定我们"名单稳定化"或"cb2[2] 重写"的 hook 位置 |
| T2 | 材质 PS dcl 是否含 t103 + comparison sampler | grep _disasm dcl_resource/dcl_sampler t103 | 补丁能否复用引擎绑定 |
| T3 | kSHADOWMAPS SRV 格式（R16_UNORM? comparison view?） | SLF 渲影纹理创建代码 + engine CreateShaderResourceView 观察 | SampleCmp 合法格式 |

---

## 4. 施工块（里程碑）

- **B0**：DXBC 解析器（token 流 → 指令表 → 可改写 → 重组）。~纯工具。
- **B1（P0 PoC）**：单变体（PS000000017384D300 系，335 文件模板）字节码改写干跑（不装游戏，离线验证 token 合法性 + D3DDisassemble 回读语义）。**里程碑 = 改后反汇编可见"逐灯 test"指令**。
- **B2**：游戏内单材质实测（挑 1 个常用材质变体强制 patch → 24 灯房看该材质表面阴影全对/不闪/不黑屏）。
- **B3**：utility pass no-op（100423/107141 detour，CS 同款）+ 引擎 mask 语义隔离。
- **B4**：绑定通道（新增 SRV/cbuffer + ResetState 时对材质 PS 补绑，**不覆盖引擎槽**）。
- **B5**：全 727 变体自动化 patch + 回归（材质无退化/ENB 正常/全场景无崩）。

验证每块都遵循"一次一变量 + 日志数据说话"。

---

## 5. 风险表

| 风险 | 等级 | 缓解 |
|------|------|------|
| DXBC 跳转/token 改写破坏 shader → 黑屏/崩 | 高 | B1 离线验证先行；单材质小步实测；随时回滚（git 无回退——手动反向替换） |
| 新增绑定槽与 ENB/其它 shader mod 冲突 | 高 | 只绑高位空槽；P1c-3 BindCSConstantBuffers 覆盖 b4/b12 前科 = 严禁覆盖引擎槽 |
| cbuffer 动态索引（仿 cb2）在部分 GPU 不支持 | 中 | 优先 StructuredBuffer SRV（无动态索引限制） |
| 渲影相机矩阵质量（扩展灯 DEF 未知）→ proj flags=0 | 中 | fix20r 日志确认 24 灯全 valid；不足则修 UpdateCamera 链 |
| 464 个非标准 t14 文件漏网 → 局部材质仍闪 | 低 | P0 只承诺 727 模板；漏网清单化后续补 |
| 与 fix20 crash 同源的调度器并发 | 低 | 渲影保持我们 dispatch；B3 no-op 前确认 maskIndex 语义 |

---

## 6. 材料索引

- 反汇编：`.../SKSE/shader_dump/_disasm/*.asm`（2720）| 聚类脚本：`slf_b_templates_v2.py` + `slf_b_templates_v2.json`
- 语义样本：PS00000002B0697D10.asm（t14 段 255-432 行）；PS000000017384D300.asm（模板 #1）
- CS 参考：open-shaders `src/Features/LightLimitFix/ShadowEngineHooks.cpp`（100423/107141 no-op、CalculateActiveLightsForSurface、GetShadowMask 族）
- SLF 代码：Scheduler.cpp（数据通道 529-620、扩展调度 1113-1160）、HookUtil.h:97-123、ShaderReplace.cpp:29-75（CreatePixelShader hook）
- 崩溃档案：crash-2026-09-04-16-41-43.log（fix20 引擎 dispatch 必崩实证）
