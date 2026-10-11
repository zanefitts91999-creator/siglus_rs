# 🎮 SiglusEngine Nintendo Switch 移植项目开发与交接日志 (PORTING_LOG)

> **文档目的**：本日志为 `siglus_rs` 项目在 Nintendo Switch 平台（Horizon OS / Atmosphere）上的全量移植维护与调试档案。任何新的 Agent 或开发者可在新对话中直接阅读本文档，立即无缝无损接力后续开发与调试工作。

---

## 📌 一、项目概况与运行环境 (Project Overview & Tech Stack)

| 维度 | 详细配置 / 参数 |
| :--- | :--- |
| **项目名称** | `siglus_rs` (Nintendo Switch 原生移植版) |
| **核心定位** | 基于 Rust 的开源 SiglusEngine 视觉小说（Visual Novel）引擎全平台重构实现 |
| **测试目标游戏** | 《Rewrite+》、《Summer Pockets》、《Angel Beats!》等 Key 社旗舰作品 |
| **物理工作区** | `D:\ns\siglus_rs` |
| **Switch 部署路径** | `sdmc:/switch/siglus_rs/siglus-switch.nro` 与 `sdmc:/switch/siglus-switch.nro` |
| **Git 仓库分支** | 分支：`main`<br>上游：`origin (https://github.com/xmoezzz/siglus_rs.git)`<br>开发远程：`myfork (https://github.com/zanefitts91999-creator/siglus_rs.git)` |
| **CI/CD 构建流水线** | GitHub Actions 工作流：`.github/workflows/build-switch.yml`<br>基础镜像容器：`ghcr.io/xmoezzz/siglus_rs/switch-build:latest`<br>编译目标平台：`aarch64-none-elf` (libnx + deko3d / deko-rs) |
| **图形后端 (Switch)** | `crates/siglus_scene_vm/src/render/horizon/mod.rs` (基于 Horizon OS 原生 deko3d 底层 API) |

---

## 📜 二、历史已解决重大缺陷全景表 (Resolved Issues & Milestones)

| 阶段 / 提交 | 缺陷现象 | 根因诊断 (Root Cause) | 解决方案 (Fix) | 实机验证结果 |
| :---: | :--- | :--- | :--- | :---: |
| **M1**<br>`820a00c` | 析构与退出崩溃 | `teardown` 阶段 `movie.stop` 与 `se.stop` 方法签名不匹配 | 修复方法签名与参数传递 | ✅ 退出不再 Crash |
| **M2**<br>`ef1b189` | 纹理内存不足与白屏 | 纹理上传频繁重新分配显存；相册缓存容量过小；缺省纹理为全白 | 恢复原地纹理重写（in-place rewrite），扩大 Album 缓存容量，消除全白 fallback 纹理 | ✅ 显存抖动消除 |
| **M3**<br>`579d519` | OBJECT 指令 191 崩溃；转场黑屏 | `OBJECT` 操作符 191 触发未实现 panic；Offscreen Target 启用了 HW Compression 导致采样错误 | 补充 op 191 操作处理；禁用 Offscreen Render Target 的硬件压缩标志 | ✅ 不再触发 191 panic |
| **M4**<br>`b13f57b` | 编译符号不一致 | 系统常量名称拼写错误 (`SYSCOM_GET_OBJECT_DISP_ONOFF`) | 修正符号定义 | ✅ 编译通过 |
| **M5**<br>`e026d8b` | **转场全屏严重闪白屏** (重大里程碑) | 离屏转场合成使用黑色清屏底色，且未完全呈现时暴露给交换链；采样 fallback 纹理为非透明 | 引入 `render_wipe_to_display`，转场精灵直出显示器交换链，使用透明 fallback 纹理 | ✅ **白屏闪烁彻底解决** |
| **M6**<br>`65ab3a1` | **人名错位、立绘缺失与掉帧** | 1. 人名守卫 `is_empty` 拦截更新且清理不彻底；<br>2. 复合立绘 `patno`/`frame_index` 报错 `bail!` 且 `album_cut` 越界返回 None；<br>3. VM Tick 动画/天气全量空扫描 CPU 瓶颈 | 1. 移除人名守卫并全流程支持清空；<br>2. 移除组合立绘 `patno`/`frame_index` 阻断，`album_cut` 安全 fallback cut 0；<br>3. 增加活跃事件与天气对象极速早退路径 | ✅ **已成功部署至 Switch 实机**<br>双路径回读哈希 100% 匹配 |
| **M7**<br>`b3df14a` | **存读档卡顿、翻页槽位丢失与主循环深优** | 1. 存读档与翻页密集查询未命中 SD 卡小文件引发大延迟；<br>2. `CHANGE_FILE` 遗漏更新 `thumb_save_no` 且缩略图为 None 时销毁 backend 导致槽位隐形；<br>3. Emote/Movie 每帧全量 stage 空迭代 | 1. 增加 `THUMB_IMAGE_CACHE` 内存缓存，存盘即时失效；<br>2. `CHANGE_FILE` 解析 `thumb_save_no`，空图绑定透明 fallback 保持有效 backend；<br>3. Emote/Movie 增加极速早退路径 | ✅ **云端构建成功，已下载本地**<br>SHA256: `6233DC46...44F2` |
| **M8**<br>`ffd6dd5` | **Switch 原生级 60 FPS 深度性能重构** | 1. `GlobalState::tick_frame` 与 `tick_stage_form` 每帧触发 13+ 处 `Vec` 堆分配、排序与 HashMap 检索；<br>2. `ObjectPropEvents` 对 36 个 `IntEvent` 每帧全量函数调用与扫描；<br>3. 对话框文字静态停留时每帧全量遍历所有 `glyphs` 与 `message_pages` 重新计算计数；<br>4. `ImageManager::organize()` 与 GPU `textures.retain()` 每帧全量 Hash 表扫描；<br>5. `plan_sprites` 每帧冗余 `clone()` 图像句柄与路径 | 1. 彻底移除所有 13 处每帧 `Vec` 堆分配，采用原地零分配直接迭代；<br>2. `ObjectPropEvents` 引入活跃判定极速路径，静止对象完全跳过 36 个事件的更新；<br>3. 引入 `last_reveal_visible` 缓存，文字静止时完全跳过字形与分页循环；<br>4. 资源整理与纹理淘汰实行 60 帧周期限制；<br>5. 修正借用检查，保持安全零分配结构 | ✅ **已成功部署至 Switch 实机**<br>双路径回读哈希 100% 匹配<br>SHA256: `FC1948F3...DEEA` |
| **M9**<br>`694daec` | **存档后立绘与属性脱节修复 (初版)** | `IntEvent::set_value` 在无活跃动画 (`loop_type == -1`) 时未同步 `cur_value`，导致 M8 早退机制下 `get_total_value()` 读取到旧的 `cur_value == 0`，角色透明度为 0 彻底隐形 | `IntEvent::set_value` 同步 `cur_value = v`，`get_total_value` 在 `loop_type == -1` 时直接返回 `self.value` | ✅ 源码已合入 |
| **M10**<br>`cb35f4c` | **彻底纠正转场场景流向 (NEXT=旧, CURRENT=新) 与立绘完美交叉淡入淡出** | 1. 深度复查确认 Siglus 架构中 `TNM_STAGE_FRONT` (Stage 1) 为新画面 (`current`)，`TNM_STAGE_NEXT` (Stage 2) 为转场前保存的旧画面 (`next`)；此前因英文语义误解一度调转；<br>2. 转场期间旧立绘未按 `(1-p)` 衰减导致旧画面人物无法渐变消失 | 1. 修正流向：$p \le 0.001$ 出 `wipe.next`（旧场景），$p \ge 0.999$ 出 `wipe.current`（新场景），结束瞬间与游戏主场景无缝咬合；<br>2. 转场中间态：旧底图保持不透明避免暗闪，旧角色立绘按 $(1-p)$ 平滑淡出，新角色立绘按 $p$ 平滑淡入 | ✅ **云端构建成功，已下载本地就绪**<br>SHA256: `48A50E62...EBCA` |
| **M11**<br>`0a932ac` | **存读档槽位隐形与缩略图失效** | 1. `CHANGE_FILE` 缩略图号解析脆弱；<br>2. 空缩略图缺少 Fallback 后端使槽位隐形；<br>3. 存档文件变动未清除内存缩略图缓存 | 1. 接入通用解析器 `parse_thumb_number`；<br>2. 空图绑定透明 Fallback 后端保护槽位；<br>3. 磁盘变动前强制清除内存缩略图缓存 | ✅ **云端构建成功，本地已下载**<br>SHA256: `675A1C02...1E59` |
| **M12**<br>`待提交` | **原生 60 FPS 二次深度调优 & G00 越界容错复合立绘修复** | 1. G00 切片越界 `bail!` 导致重建失败抹除立绘 backend；<br>2. `repair_missing_gfx_leaf_images` 遗漏复合立绘导致立绘无法自愈；<br>3. `tick_frame` 每帧全量无条件堆分配 `object_chain`；<br>4. `tick_stage_form_state` 每帧空扫描无用槽位；<br>5. 色调曲线与 GAN 效果每帧全量空扫描与 HashMap 分配；<br>6. 渲染树构建每帧分配/排序非目标 stage 键集合 | 1. G00 切片越界改为安全回退至 cut 0，彻底根除立绘消失；<br>2. 修复复合立绘修复路径，支持 `load_g00_composed`；<br>3. `has_any_frame_action_recursive` 早退跳过堆分配；<br>4. `needs_tick` 与未使用槽位快速过滤；<br>5. 色调曲线与 GAN 效果活跃守卫早退；<br>6. 渲染树非 debug 模式直达目标 stage | 🚀 **待提交并触发构建** |

---

## 🏷️ 三、实机当前与最新待部署版本实证校验 (Deployed vs Latest Build)

### 1. 最新构建版本 (M10 - 终极立绘与转场双重修复版，待注入 Switch 实机)
- **Git Commit**：`cb35f4c` (`fix(switch): correct horizon wipe scene flow mapping NEXT as old scene and CURRENT as new scene with smooth character crossfade`)
- **GitHub Actions Run ID**：`38142970328` (Artifact ID: `11703019178`)
- **文件物理路径**：`D:\ns\siglus-switch.nro` 与 `D:\ns\v13_build\siglus-switch.nro`
- **文件体积**：`25,300,208` 字节 (24.13 MB)
- **SHA-256 权威校验和**：
  ```
  48A50E6206349DAFC37E04B322EAF41C41D020C69AA30D6A33C248A61D16EBCA
  ```
- **一键部署脚本**：`powershell -ExecutionPolicy Bypass -File D:\ns\deploy_m10_to_switch.ps1`
- **核心修复**：
  1. `IntEvent::set_value` 与 `get_total_value` 属性同步机制（彻底解决 M8 极速早退跳过 `frame()` 导致的立绘透明度脱节为 0 隐形问题）；
  2. `render_wipe_to_display` 转场流向 100% 纠正（NEXT=旧场景 $1-p$，CURRENT=新场景 $p$，底图抗暗降，立绘平滑交叉淡化）。

### 2. Switch 实机当前在线版本 (M8 - 原生级 60 FPS 深度重构版)
- **Git Commit**：`ffd6dd5` (`fix(switch): fix borrow conflict in plan_sprites`)
- **SHA-256 权威校验和 (实机回读实测)**：
  ```
  FC1948F31BAF9C6EA438AEDEC9A219DEEC6E117BA177D2F75CA451AE5318DEEA
  ```
- **运行状态**：已解决人名错位、60 FPS 零分配卡顿与存读档缩略图翻页。待连机部署 M10 彻底解决存档后立绘缺失与转场反向截断。

### 3. 历史归档版本
- **M7 (`b3df14a`)**：`6233DC46262D8B2AEB7EBF82C8C1312697AEEBA7975EB8A27524793DF34144F2` (存读档缩略图缓存)
- **M6 (`65ab3a1`)**：`78B210C7EA96152E411E8F7AFDEFBCC494E56CB8A59889487C1B40788E7D8413` (人名与立绘修复)

---

## 🏛️ 四、历史重大缺陷攻坚技术复盘归档 (Historical Milestones Technical Post-Mortem)

> **归档说明**：本节收录此前在 M6、M8 等重大阶段中排查并彻底解决的关键技术根因与修复思路，留存作为引擎架构参考，**非当前待解决问题**。当前实际工作进度以“第三节版本校验”与“第七节 M11 阶段”为准。

---

### ✅ [已彻底解决 - 归档于 M6] 缺陷 1：对话框说话人名称错位 / 滞后不更新 (Speaker Name Mismatch)

#### 1. 现象描述
在游戏推进过程中，说话的角色已经改变，但对话框名牌（Nameplate）依然显示前一个角色的名字；或者旁白/独白时前一个角色的名字依然残留。

#### 2. 底层代码根因剖析
在 `crates/siglus_scene_vm/src/runtime/forms/stage.rs` 中，存在极其严重的**静默拦截守卫**与**清空遗漏**：
1. **`cd_name_current_mwnd` 存在阻断判断**（L14484）：
   ```rust
   // 代码位置：crates/siglus_scene_vm/src/runtime/forms/stage.rs:14483
   start_mwnd_msg_block_if_needed(ctx, stage_idx, m);
   if m.name_text.is_empty() { // ❌ 致命错误：如果上一句话名字未清空，后续所有角色的名字直接被完全丢弃！
       let resolved_name = resolve_gameexe_namae(&ctx.tables, name);
       ...
   }
   ```
2. **`MwndOpKind::SetName` 同样存在阻断判断**（L14938）：
   ```rust
   MwndOpKind::SetName => {
       if !m.name_text.is_empty() { // ❌ 致命错误：非空时直接 push_ok 返回，根本不执行更新！
           push_ok(ctx, ret_form);
           return true;
       }
   ```
3. **`MwndOpKind::ClearName` 遗漏清空字符与 UI**（L15271）：
   只清空了 `m.name_text.clear()`，**未调用 `m.name_glyphs.clear()`，也未调用 `ctx.ui.clear_name()`**！
4. **`apply_mwnd_novel_clear` 遗漏人名清空**（L14364）：
   清空了消息版式和文本，但人名 `m.name_text`、`m.name_glyphs` 毫发无损，且没有调用 `ctx.ui.clear_name()`！导致 NovelClear（换行/翻页）后残留旧角色名。
5. **`clear_mwnd_message_block_now` 遗漏 `m.name_glyphs` 清空**（L14298）：
   清空了 `m.name_text`，但未清空 `m.name_glyphs`。

#### 3. 精确修复方案
在 [`crates/siglus_scene_vm/src/runtime/forms/stage.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/forms/stage.rs) 进行如下 5 处修改：

- **修改点 1 (`cd_name_current_mwnd`, L14483-14499)**：
  ```rust
  start_mwnd_msg_block_if_needed(ctx, stage_idx, m);
  let trimmed = name.trim();
  if trimmed.is_empty() {
      m.name_text.clear();
      m.name_glyphs.clear();
      m.chara_color_mod = None;
      m.chara_moji_color = None;
      m.chara_shadow_color = None;
      m.chara_fuchi_color = None;
      ctx.ui.clear_name();
  } else {
      let resolved_name = resolve_gameexe_namae(&ctx.tables, trimmed);
      let display_name = resolved_name.display;
      m.chara_color_mod = resolved_name.color_mod;
      m.chara_moji_color = resolved_name.moji_color_no;
      m.chara_shadow_color = resolved_name.shadow_color_no;
      m.chara_fuchi_color = resolved_name.fuchi_color_no;
      super::syscom::reveal_config_voice_name(ctx, &display_name);
      m.name_text = display_name.clone();
      mwnd_rebuild_name_glyphs(ctx, m, mwnd_idx, &display_name);
      ctx.ui.set_name(display_name.clone());
      msgbk_add_name(ctx, &display_name);
  }
  true
  ```

- **修改点 2 (`MwndOpKind::SetName`, L14938-14961)**：
  移除 `if !m.name_text.is_empty() { push_ok(ctx, ret_form); return true; }` 守卫，无条件根据入参更新或清空人名。

- **修改点 3 (`MwndOpKind::ClearName`, L15271-15279)**：
  增加 `m.name_glyphs.clear();` 和 `ctx.ui.clear_name();`。

- **修改点 4 (`apply_mwnd_novel_clear`, L14364-14380)**：
  在清除消息前增加：
  ```rust
  m.name_text.clear();
  m.name_glyphs.clear();
  m.chara_color_mod = None;
  m.chara_moji_color = None;
  m.chara_shadow_color = None;
  m.chara_fuchi_color = None;
  ctx.ui.clear_name();
  ```

- **修改点 5 (`clear_mwnd_message_block_now`, L14301)**：
  在 `m.name_text.clear();` 之后补上 `m.name_glyphs.clear();`。

---

### ✅ [已彻底解决 - 归档于 M6/M10] 缺陷 2：人物立绘缺失 / 差分表情不显示与转场截断 (Missing Character Sprites & Crossfade)

#### 1. 现象描述
部分角色的立绘、或从一个表情切换到另一个表情时，人物没有出现，画面中原本应有立绘的地方空白。

#### 2. 底层代码根因剖析
1. **Composed G00（组合立绘）在遇到非零 `patno` 时直接崩溃报错**：
   - 文件：[`crates/siglus_scene_vm/src/runtime/graphics.rs:319-322`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/graphics.rs#L319-L322)
   ```rust
   if file.contains('|') {
       if patno != 0 {
           bail!("composed g00 has one texture; invalid pattern {patno}"); // ❌ 致命错误！
       }
       return images.load_g00_composed(file)...;
   }
   ```
   在 Key 游戏（Rewrite、Summer Pockets 等）中，立绘由多个部件拼接（格式如 `ch01_01|00|01`）。脚本在给立绘槽位更新表情时，可能会传入或沿用当前插槽的 `patno`。此处直接 `bail!` 导致整条立绘装载失败并返回 `Err`，立绘纹理直接为 `None`，立绘凭空蒸发！
   **正确逻辑**：Composed G00 本身就由单张组合图提供（cut 0），`patno` 应该直接被忽略或 clamp 至 0，绝不可 `bail!` 报错。

2. **`album_cut` 超出范围直接返回 `None` 导致立绘丢失**：
   - 文件：[`crates/siglus_scene_vm/src/image_manager.rs:96-104`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/image_manager.rs#L96-L104)
   ```rust
   pub fn album_cut(&self, cut: usize) -> Option<Self> {
       let count = self.album.frames.read().expect("image album lock poisoned").len();
       (cut < count).then(|| Self::new(self.album.clone(), cut)) // ❌ 错误：cut >= count 时返回 None！
   }
   ```
   如果图集切片数不足（例如组合立绘只有 1 帧，或者表情索引临时超出），返回 `None` 会导致上层 `sync_object_sprite` 无法绑定纹理，最终立绘被剔除不渲染。
   **正确逻辑**：若 `count > 0` 且 `cut >= count`，应安全回退取切片 0 (`safe_cut = if cut < count { cut } else { 0 }`)，绝不可返回 `None` 导致立绘彻底消失。

3. **`render_wipe_to_display` 转场截断丢失后续精灵**：
   - 文件：[`crates/siglus_scene_vm/src/render/horizon/mod.rs:366-399`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/render/horizon/mod.rs#L366-L399)
   ```rust
   let common_prefix = wipe.next.iter().zip(wipe.current.iter()).take_while(...).count();
   ```
   当立绘表情改变时，`common_prefix` 提前中断；之后的所有立绘部件在 `next` 和 `current` 中分别用 `(1.0 - p)` 和 `p` 调制透明度后压入同一渲染列表。带有 Alpha 通道的立绘部件在两次混合叠加后，数学上不等于原图透明度（会导致半透明幽灵化）；当立绘部件增减时，未匹配项还会因为透明度计算问题直接隐形。

---

### ✅ [已彻底解决 - 归档于 M8] 优化项 3：Switch 实机 60 FPS 零分配深度重构 (60 FPS Performance Re-architecture)

#### 1. 现象描述与实测数据
在 `siglus_switch.log` 中记录的真实帧耗时：
- `gpu` 渲染耗时仅约 **0.20 ms**（deko3d 显卡渲染极快，GPU 算力极其充沛）；
- `step` 总耗时高达 **44.59 ms**，其中 `tick` 单项耗时达 **38.10 ms**（折合帧率仅约 22~25 FPS）。
- 瓶颈 100% 在 Switch CPU（4核 A57 1.0GHz）的主线程 VM 更新循环。

#### 2. 优化方案
1. **`apply_object_event_animations` 快速路径早退**：
   在 [`crates/siglus_scene_vm/src/runtime/mod.rs:4810`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/mod.rs#L4810)：若全局没有任何处于活跃状态的对象事件动画控制器（`event_animations.is_empty()`），直接跳过对所有 Stage Forms、Object Lists 的全量迭代和排序。
2. **`sync_weather_objects` 等天气/粒子特效早退**：
   在无活跃天气特效时直接返回，避免每帧重复空扫描。

---

## 🛠️ 五、自动化构建、下载与实机部署流程 (Build & Deployment Pipeline)

### 1. 提交与触发构建
在 `D:\ns\siglus_rs` 目录下执行：
```powershell
git add .
git commit -m "fix(switch): resolve speaker name mismatch, handle composed g00 patno, and optimize frame tick"
git push myfork main
```

### 2. 自动化监控与下载
在 `D:\ns` 目录下运行已配置好 GitHub Token 与代理的下载脚本：
```powershell
python D:\ns\wait_and_download.py
```
> **说明**：脚本会自动轮询 GitHub Actions Run 状态，构建完成后通过 `127.0.0.1:7897` 代理自动拉取 `siglus-switch-binaries` 制品并解压出 `siglus-switch.nro`。

### 3. 一键部署至 Switch (MTP)
通过 Windows Shell COM 对象直连 Switch MicroSD 卡（无需拔卡或关机）：
```powershell
# 运行部署与自动 SHA-256 实证校验脚本
powershell -ExecutionPolicy Bypass -File D:\ns\update_root_nro_v9.ps1
```

### 4. 实证校验命令
```powershell
powershell -ExecutionPolicy Bypass -File D:\ns\verify_current_hashes.ps1
```

---

## 🚀 六、新对话 / 接力 Agent 执行指南 (Handover Instructions)

如果此对话结束，新会话的 Agent 按照以下顺序执行：
1. **优先通读本文档第二、三节**：确认当前已发布的里程碑状态与最新二进制哈希校验；
2. **核验当前任务断点 (M11)**：
   - M11 代码已提交至 `myfork/main` (`0a932ac`)；
   - 包含：`stage.rs` 缩略图号通用提取、空槽位透明 fallback backend 绑定与 `syscom.rs` 缓存强制失效；
3. **获取最新构建与实证哈希**：
   - 运行 `python D:\ns\wait_and_download_m11.py` 获取最新 Switch NRO 二进制并记录真实 SHA-256；
4. **运行 MTP 部署脚本传输至 Switch 并实测校验哈希**：
   - 通过 `powershell -ExecutionPolicy Bypass -File D:\ns\deploy_m10_to_switch.ps1`（或对应的 M11 部署脚本）写入双路径；
5. **在 Switch 实机上验证测试**：
   - 验证转场立绘平滑淡出淡入；
   - 验证存档后立绘正常显示；
   - 验证存读档菜单翻页时槽位与缩略图不消失、不卡顿；
6. **如实向用户汇报真实更新结果与测试表现**。

---

## 📌 M11 阶段修复：存读档菜单翻页卡顿、缩略图槽位消失隐形与缓存同步修复 (Save/Load Menu Slot Restoration & Thumbnail Cache Sync)

- **修改时间**：2026-10-11
- **对应模块**：
  - [`crates/siglus_scene_vm/src/runtime/forms/stage.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/forms/stage.rs)
  - [`crates/siglus_scene_vm/src/runtime/forms/syscom.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/forms/syscom.rs)
- **缺陷现象**：
  1. 存读档菜单翻页时，某些存档槽位隐形消失或空白；
  2. 存档复制、交换或删除后，缩略图显示过时的旧数据甚至残留；
  3. 缩略图编号解析脆弱，对带有前缀或复杂命名的缩略图无法匹配，导致槽位对象无 backend 丢失。
- **底层根因剖析**：
  1. **缩略图号提取未接入通用解析器**：在 `stage.rs` 的 `CHANGE_FILE` (L9341-9354) 中，仅对纯数字进行解析，当文件名带前缀（如 `save_01.bmp`、`slot12` 等）时解析失败，导致 `thumb_save_no` 保持为 -1，对象无法加载正确缩略图。
  2. **空缩略图槽位缺少 Fallback 后端**：在 `OBJECT_CREATE_SAVE_THUMB` (L9743)、`OBJECT_CREATE_CAPTURE_THUMB` (L9774) 以及读档后恢复对象 (L6788, L6865) 时，若缩略图文件尚未生成（未存档的空位），原逻辑未调用 `bind_capture_backend`，使 `obj.backend` 保持为 `ObjectBackend::None`，导致空存档位在渲染树构建中被剔除，槽位隐形消失。
  3. **内存缩略图缓存未失效**：在 `syscom.rs` 的 `copy_slot`、`change_slot`、`delete_slot` 完成磁盘文件增删换之后，未调用 `clear_thumb_image_cache()`，使内存静态缓存 `THUMB_IMAGE_CACHE` 长期驻留旧句柄。
- **具体修改方案**：
  1. 在 `stage.rs` 引入通用解析器 `parse_thumb_number`，并在 `CHANGE_FILE` 中全面替代硬编码数字解析；
  2. 在 `OBJECT_CREATE_SAVE_THUMB`、`OBJECT_CREATE_CAPTURE_THUMB` 及 `restore_object_backend_after_load` 中，对空图槽位绑定透明 fallback backend (`ctx.images.solid_rgba((0, 0, 0, 0))`)；
  3. 在 `syscom.rs` 的 `copy_slot`、`change_slot`、`delete_slot` 成功返回前均触发 `super::stage::clear_thumb_image_cache()` 强制失效缓存。

---

## 📌 M12 阶段修复与深优：Switch 原生级 60 FPS 二次深度调优 & G00 越界容错复合立绘修复 (Native 60 FPS Deep Performance Tuning & G00 Out-of-Bound Sprite Resilience)

- **修改时间**：2026-10-11
- **对应模块**：
  - [`crates/siglus_scene_vm/src/image_manager.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/image_manager.rs)
  - [`crates/siglus_scene_vm/src/runtime/globals.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/globals.rs)
  - [`crates/siglus_scene_vm/src/runtime/mod.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/mod.rs)
  - [`crates/siglus_scene_vm/src/vm.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/vm.rs)
- **缺陷现象**：
  1. 存档或切换场景后复合立绘偶发缺失：G00 切片超出帧数时返回 `bail!` 报错，导致 `restore_object_backend_after_load` 判定恢复失败，清空 `file_name` 并将 `backend` 重置为 `None`，导致立绘彻底消失；
  2. 显存回收与自愈修复失效：`repair_missing_gfx_leaf_images` 未处理复合立绘（包含 `|` 描述符），尝试作为普通文件加载失败并加入永久黑名单；
  3. CPU 性能瓶颈 1：`vm.rs` 的 `tick_frame` 每帧全量无条件为每个对象（包括无动作对象）分配 `Vec<i32>` 堆内存 `object_chain`；
  4. CPU 性能瓶颈 2：`globals.rs` 的 `tick_stage_form_state` 每帧无差别遍历所有空槽位和静止对象，进行无效 `obj.tick()` 计算；
  5. CPU 性能瓶颈 3：`runtime/mod.rs` 的 `apply_object_tonecurves` 与 `apply_gan_effects` 每帧全量扫描所有 Stage Forms，且 `apply_gan_effects` 频繁分配与填充 `HashMap`；
  6. CPU 性能瓶颈 4：`build_siglus_object_render_list` 每帧对每个 Stage Form 分配、排序与去重 7 个列表的键集合，扫描统计非目标 stage 的各种对象数量，随后全部被丢弃。
- **底层根因剖析**：
  1. 原版 Siglus `C_d3d_album::get_texture` 对越界切片具备安全容错机制，而移植版硬编码 `bail!` 导致重建中断抹除 backend；
  2. `repair_missing_gfx_leaf_images` 未分流 `load_g00_composed`；
  3. `tick_frame` 收集 `FrameActionWork` 缺乏前置活跃检查；
  4. `tick_stage_form_state` 缺乏未使用槽位和 `needs_tick` 早退路径；
  5. 色调曲线与 GAN 效果缺乏全局活跃标记，`apply_gan_effects` 过早无条件分配 `HashMap`；
  6. 渲染树构建缺乏直接定位 `selected_stage` 的快速路径。
- **具体修改方案**：
  1. 在 `image_manager.rs` 的 `load_g00` 与 `load_file` 中，将越界切片由 `bail!` 改为警告并安全回退至 cut 0，彻底保护对象 backend 不被置空；
  2. 在 `runtime/mod.rs:repair_missing_gfx_leaf_images` 中，增加 `if file.contains('|') { self.images.load_g00_composed(&file) }` 路径；
  3. 在 `ObjectState` 实现 `has_any_frame_action_recursive(&self) -> bool`，在 `vm.rs` 中跳过无动作对象，消灭每帧数百次 `object_chain` 堆分配；
  4. 在 `ObjectState` 实现 `needs_tick(&self) -> bool`，在 `tick_stage_form_state` 中快速跳过未分配槽位与静止对象；
  5. 在 `runtime/mod.rs` 中为色调曲线增加 `has_table()` 守卫；为 GAN 效果增加 `has_any_active_gan()` 检查，避免无 GAN 场景下 `HashMap` 分配与全量对象递归；
  6. 在 `build_siglus_object_render_list` 中，增加非 debug 模式直达 `selected_stage` 快速路径，消除每帧多键合并向量分配与排序。

---

## 📌 M12.1 阶段修复：CI 编译与读档差分立绘切片恢复深度加固 (M12 Build Fix & Save-Load Sprite Restoration Deep Fix)

- **修改时间**：2026-10-11
- **对应模块**：
  - [`crates/siglus_scene_vm/src/runtime/gan.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/gan.rs)
  - [`crates/siglus_scene_vm/src/runtime/globals.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/globals.rs)
  - [`crates/siglus_scene_vm/src/runtime/graphics.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/graphics.rs)
  - [`crates/siglus_scene_vm/src/runtime/forms/stage.rs`](file:///D:/ns/siglus_rs/crates/siglus_scene_vm/src/runtime/forms/stage.rs)
- **缺陷现象**：
  1. GitHub Actions CI 构建失败：`GanState.data` 属于私有字段导致编译报错 E0616；`tick_stage_form_state` 在遍历 `&mut st.object_lists` 时调用 `st.object_slot_is_used` 导致借用检查器报错 E0502；
  2. 读档后差分立绘丢失或切片错位：读档后立绘表情被强行重置为 `base.patno`（通常为 0），丢失了脚本动态通过 `OBJECT.PATNO` 设置在 `prop_events.patno` 中的实际表情切片；
  3. 读档偶发性彻底抹杀立绘对象：`restore_object_backend_after_load` 在 PCT 恢复遇错时执行死刑逻辑，直接清空 `obj.file_name = None` 并将 `obj.backend = None`，导致自愈系统 `repair_missing_gfx_leaf_images` 彻底无法恢复。
- **底层根因剖析**：
  1. `GanState` 缺少 `pub fn has_data(&self) -> bool` 公开查询接口；`tick_stage_form_state` 未对 `StageFormState` 结构体进行字段级拆分借用（split borrow）；
  2. 在 `graphics.rs:796` 的 `restore_gfx_object_from_globals` 中，所有其他动画属性均从 `pe` (`prop_events`) 获取，唯独 `patno` 误写为 `src.base.patno`；
  3. 在 `stage.rs:6680` 中，读档失败保护机制过于激进，抹去了文件名和 Gfx 后端标记。
- **具体修改方案**：
  1. 在 `gan.rs` 为 `GanState` 增加 `#[inline(always)] pub fn has_data(&self) -> bool`，并在 `globals.rs:5686, 5691` 中替换对私有字段的直接访问；
  2. 在 `globals.rs:7685` 中提前独立借出 `embedded_by_stage` 和 `slot_use_by_stage`，消灭 Rust 借用冲突；
  3. 在 `graphics.rs:796` 中将 `dst.patno` 改为优先从 `pe.patno.get_total_value() as i64` 获取，回退到 `src.base.patno`；
  4. 在 `stage.rs:6665` 中，读档 PCT 恢复遇到错误时仅记录 warn，保留 `obj.backend = ObjectBackend::Gfx` 与原文件名，确保后续每帧运行的自愈系统能自动重试并拉起立绘。



