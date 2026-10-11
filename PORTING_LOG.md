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
| **M6**<br>待提交 | **人名错位、立绘缺失与掉帧** | 1. 人名守卫 `is_empty` 拦截更新且清理不彻底；<br>2. 复合立绘 `patno`/`frame_index` 报错 `bail!` 且 `album_cut` 越界返回 None；<br>3. VM Tick 动画/天气全量空扫描 CPU 瓶颈 | 1. 移除人名守卫并全流程支持清空；<br>2. 移除组合立绘 `patno`/`frame_index` 阻断，`album_cut` 安全 fallback cut 0；<br>3. 增加活跃事件与天气对象极速早退路径 | 🔄 源码已完成修复，等待 CI 构建与实机校验 |

---

## 🏷️ 三、实机当前已部署版本实证校验 (Current Deployed Release)

- **Git Commit**：`e026d8b` (`fix(switch): render wipes directly to display swapchain and use transparent fallback texture`)
- **文件物理路径**：`D:\ns\siglus-switch.nro`
- **文件体积**：`25,296,112` 字节 (24.12 MB)
- **SHA-256 校验和**：
  ```
  27CD06DCFD3A0A03B2E4D6C4E6FB8A03EBAC4EF9968D22B191ADFEDBDFE88D98
  ```
- **实机运行状态**：游戏完全可进，立绘背景正常显示，彻底告别白屏闪烁，但存在下述待解决问题。

---

## 🔍 四、待解决核心问题深度剖析与源码修复方案 (Pending Issues & Fixes)

用户最新实测反馈聚焦在三大问题：
1. **对话框话与人名错位**（说话的人与显示的名字不匹配、名字卡在上一人未更新）；
2. **人物立绘/差分缺失**（部分人物立绘、表情差分消失或隐形）；
3. **游戏运行卡顿**（Switch 实机帧率约 20~25 FPS，存在 CPU 瓶颈）。

---

### 🔴 缺陷 1：对话框说话人名称错位 / 滞后不更新 (Speaker Name Mismatch)

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

### 🔴 缺陷 2：人物立绘缺失 / 差分表情不显示 (Missing Character Sprites)

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

### 🟡 优化项 3：Switch 实机运行卡顿 / CPU VM Tick 耗时偏高 (Performance Lag)

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
1. **优先通读本文档**：定位至第四节的代码修改方案；
2. **实施代码修改**：
   - 编辑 `crates/siglus_scene_vm/src/runtime/forms/stage.rs` 修复人名错位问题；
   - 编辑 `crates/siglus_scene_vm/src/runtime/graphics.rs` 和 `crates/siglus_scene_vm/src/image_manager.rs` 修复立绘与差分表情丢失问题；
   - 编辑 `crates/siglus_scene_vm/src/runtime/mod.rs` 添加动画更新快速跳过；
3. **提交并推送至 `myfork/main`**；
4. **运行 `wait_and_download.py` 等待构建完成**；
5. **运行 MTP 脚本传输至 Switch 并实测校验哈希**；
6. **如实向用户汇报更新结果**。
