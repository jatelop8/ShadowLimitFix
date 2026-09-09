# SLF-B B2 设计增补：逐灯阴影采样语义（CS 权威对照）

日期：2026-09-04  B1 离线闭环后。来源：open-shaders LightLimitFix.hlsli
（GetShadowLightShadow / GetSpotlightShadow / GetOmnidirectionalShadow /
SampleParaboloidShadow，RenderDoc 验证过 kSHADOWMAPS slice 内容）。

## 1. 变体 384D300 的阴影消费语义（引擎原版，实测反汇编）
- 灯循环（≤ min(cb2[29].x,7) 盏）：shadow 因子在 **r2.w**（`dp4 r2.w, cb2[2].xyzw, icb` 选通道 →
  `dp4 r2.w, r4.xyzw, icb` 查 t14 4 通道 mask），L110 `mul r8.xyz, r2.wwww, cb2[i+22]` 乘漫反射
- 引擎只烘焙 ≤4 盏进 t14（screen-space mask，类型无关），>4 盏无阴影 = 闪根因
- SLF-B = 把该单元换成**逐灯实时深度采样**（每灯查 kSHADOWMAPS 自己 slice）

## 2. 采样语义分型（铁律，CS 实证）
每灯记录必须带 type 标志（ShadowLightParam.x）：
- **0 = spot/frustum（方向/聚光）**：positionLS = mul(ShadowProj, worldPos4)；透视除 w →
  uv = xy*0.5+0.5；深度 = positionLS.z（引擎深度），bias 缩放；可选 spotFalloff = saturate(1-|xy|²)
- **1 = hemisphere（半球，单抛物面铺满 slice）**：只渲 +Z；behind（positionLS.z<0）无数据 → shadow=1.0
- **2 = omni（双抛物面，一 slice 上下堆叠）**：+Z 在上半 y∈[0,0.5]，-Z 在下半 y∈[0.5,1] 镜像
- 抛物面 UV（两型共用）：dir = normalize(normalize(posLS.xyz) + (0,0,±1))；
  uv = dir.xy/dir.z*0.5+0.5；omni 再 y 压缩（lower: y = 1-0.5y；upper: y = 0.5y）
- 抛物面深度 = saturate(length(posLS.xyz) / radius)（ShadowLightParam.y = radius）− bias
- 采样目标：纹理 2darray（SLF 的 kSHADOWMAPS 扩展槽 0..127，或 CS 的 ShadowMaps/ShadowAtlas）
  ——SLF 渲影 = 一灯 1 slice（dir/spot）或 1 slice（omni 上下堆/hemi 全片），slice 号进记录

## 3. t102 LightRec 契约（96B，与现有 Scheduler g_shadowLights 对齐改造）
```
offset 0   float4 proj[0]   // spot: view-proj row0；omni/hemi: light→light-space 旋转行（见下）
offset 16  float4 proj[1]
offset 32  float4 proj[2]
offset 48  float4 proj[3]   // 4 行 = mul(rot|−rot·pos, worldPos) 仿射 → posLS.xyz 为 light-space 偏移
offset 64  float4 posrad    // xyz = light pos（调试）；w = radius（抛物面深度归一化）
offset 80  float4 slotflags // x = type(0 spot/1 hemi/2 omni)，y = enabled(0/1)，z = slice(0..127)，w = bias
```
- 点光：proj 4 行存 rotation（世界→光本地，光位置平移消掉）= CS ShadowProj for omni 语义
- spot/方向：proj = 完整 view-proj
- bias 建议起步固定 -0.001（投影）/抛物面用半径比例（后续 PCF 再调）
- 一灯占 1 slice：SLF dispatch 渲影目标已是一灯 1+ slice（parabolic 双半同片上下叠？NO——
  fix19n scan 实证 20-30 slice 有内容 = 我们渲的 24 灯；渲影侧 parabolic 是否上下叠半取决于
  BSShadowParabolicLight::Render 的 viewport/投影设定——B2 游戏实测时用 probe 读回确认叠法）

## 4. B2 落地顺序（一次一变量）
- B2a 模板 v2：HLSL 双路径（spot + paraboloid(hemi/omni)）编译 → WARP CPS 验证 → 寄存器审计
  （payload 全部 r≥引擎 dcl_temps，目标变体 gate = dcl_temps ≤ 9 保证 r9.. 空闲）
- B2b C++ 移植：定位单元→切→拼 payload（v2 恒定字节，可预生成头文件数组）→ 升 dcl_temps →
  **重算 DXBC obfuscated MD5**（算法=dxbc_hash.py，C++ 移植）→ HookCreatePixelShader 注入
- B2c 离线一致性：C++ 产物 vs Python 产物 byte 比对（同一 dump 输入）
- B4 运行时数据：Scheduler 每帧填 t102（96B×N 结构化，含 proj/radius/type/slice/bias）+
  PSSetShaderResources 绑 s15/t102/t103（材质 pass，EngineFix 时机），gating 补丁 PS
- 验收：3 盏灯（房内实测灯型以日志 [CN]/[PRE] 为准）阴影正确稳定不闪

## 5. 风险与已知项
- 点光 slice 内容叠法（上下堆 vs 各占一 slice）未定 → B2 游戏前用 probe-clear 同款读回确认
- 引擎渲影 dispatch 崩溃前科：**禁 SLF_SKIP_VANILLA_DISPATCH=0**；SLF dispatch 保持唯一生产者
- t103 SRV 材质 pass 是否已绑（T3）→ B4 无条件绑（PSSetShaderResources 补绑，勿覆盖引擎槽
  0-14 现有资源；用 102/103/15 槽）
- PCF：v2 先单 tap 硬阴影（bias 起步），PCF 8-tap + 旋转表后续再加（防 acne 抖动留到稳定后）
