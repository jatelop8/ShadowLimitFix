# SLF-B4b：引擎 cb2 批序 vs t102 序错位——闪烁根因实证（2026-09-05）

## 数据
用户站桩 1 小时，日志 605MB / 2.1M 行，staging readback 92,041 组观测（每 64 pass 一次）。

## 铁证 1：引擎批硬上限
- cb2[29].x (lights) ∈ 0..7，**从不 >7**
- cb2[29].y (shadow) ∈ 0..4，**从不 >4**
→ 引擎 CPU 把每 surface 阴影数 clamp 到 4（原版 t14 四通道时代的硬限制）
→ 渲影侧渲了 24 盏（实证内容真实），但**引擎材质批从未包含第 5..24 盏**

## 铁证 2：cb2[2] 通道映射语义实证
chmap=(0,0,0,0) 55k 次 + (3,0,0,0)/(1,2,0,0)/(2,3,0,0)/(0,1,2,3)...
= 引擎 CPU 每帧填"批内照明灯 i -> t14 通道号"（文档 §1.2 模板 dp4 cb2[2].xyzw, icb[loopIdx] 的 icb 值来源）
→ 引擎批内灯序是引擎自己排的（per-surface 选灯），非 accumulator 序

## 铁证 3：批序 vs t102 序错位（闪烁机制）
同帧三行对比（92,041 组，抽样 12/12 全 DIFF）：
  cb2 light[0] = (8.7,-20.1,-824)     <- 引擎批首灯（每 surface 变）
  LightRec[0]  = (-1223.7,-543.4,565.9) <- t102 第 0 条 = accumulator 序（恒定）
B2b payload 反汇编确认用**引擎材质循环变量**索引 t102（ftoi r18.x,r26.zzzz=counter → ld_structured t102[r18.x]）
→ 引擎循环遍历 cb2 批（含非阴影灯、序=引擎选灯），t102 是渲影 accumulator 序 → 两序无对应
→ shadow test 用错灯的数据 = 阴影形状错乱/张冠李戴 = 闪烁

## CS 权威解法（对照 ShadowEngineHooks.cpp:732-870）
CS hook CalculateActiveNonShadowCasterLights (100997/107784, AE RVA 0x14FA570) 整函数替换：
- lights[0]=sun
- Step1/2: accumulator+扩展池阴影灯全部注入（shadowCount 累加 >4）
- Step3: 非阴影灯补足
→ 引擎批序 == CS 渲影序 → 材质 idx == 渲影 idx → 消费端对齐
SLF 未接管该函数（P1a 仅验证地址）→ B4c = CS 同款 per-surface 注入

## B4c 方向
1. install_context_hook(107784 地址, 5, Hook_CalculateActiveLightsForSurface) + func+5 写 RET
2. hook 体 = CS 同款：从我们的 g_scheduledShadowLights（accumulator 序=渲影序）注入
3. 让引擎 cb2 批 = 渲影序，shadowCount 反映真实数
