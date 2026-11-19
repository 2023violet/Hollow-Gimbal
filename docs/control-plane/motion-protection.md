# FalconFOC 软件运动保护

运动保护通过 `foc_runtime_motion_config` 显式配置。位置软限位和跟随误差各自有启用开关，默认都关闭。生产接口见 [Runtime](../../runtime/include/foc_runtime.h)、[Motion Limits](../../core/include/foc_motion_limits.h) 和 [Following Error](../../core/include/foc_following_error.h)。

## 配置和模式

- 在已标定、连续、多圈的电机轴机械坐标中配置 `position_min_rad` 和 `position_max_rad`，单位 rad。上下界包含端点。区间、跨度和 `tolerance_rad` 必须有限，且上下界严格递增。容差从端点向内建立方向检查区，两侧检查区必须留有间隔。较大圈数下应留出 Float32 可分辨的余量。
- 启用任一保护时，Runtime 的运动路径必须启用，且 `limits.mechanical_frame_id` 必须非零并匹配有效机械反馈。启用 IMPEDANCE 时，其配置也须使用同一坐标身份。坐标身份包含机械轴、零点及正向。外部关节坐标需要由集成端换算。
- 跟随误差阈值为有限正数，持续时间和最大采样间隔均在 `1..0x7FFFFFFF` µs 范围。最大间隔至少覆盖 H0、H1 各自的标称周期。保护参数应由目标产品的能力、延迟及负载决定。

| 模式 | 位置软限位开启 | 跟随误差开启 |
| --- | --- | --- |
| POSITION | 拒绝界外绝对目标。方向检查覆盖位置推进、位置环速度、整形后的速度和最终 q 电流请求。实测越界时允许回到区间内。 | RUNNING 时比较有效位置目标与实测机械位置。 |
| VELOCITY | 使用可信实测位置，检查原始及整形速度和最终 q 电流请求。实测越界时允许向内的请求。 | 无位置参考，状态为 `INACTIVE`。 |
| IMPEDANCE | 拒绝界外绝对目标。检查位置、速度以及弹簧、阻尼与前馈合成并限幅后的 q 电流请求。 | RUNNING 时比较有效阻抗平衡位置与实测机械位置。 |
| CURRENT | 命令、选模和 ARM 返回等效 `UNSUPPORTED_FOR_MODE`。初始默认模式仍是 CURRENT，需要先切换。 | 单独开启跟随误差时沿用 CURRENT，监控为 `INACTIVE`。 |

进入边界容差区或已越界后，相关目标、速度或净 q 请求中任何向外分量都足以否决当拍。合法回转由调用方明确发送，不会自动把目标裁剪到端点或反推回转。位置恢复目标须位于配置区间内，速度和 q 请求也须通过同一方向检查。速度指令为零时，向外的积分输出仍会导致拒绝。

## 准入、时间和输出确认

Foreground 校验绝对位置目标。H1 使用带原始采样时间及模式、Session 的机械反馈生成提案，H0 复核实际消费的同一完整提案，按照现有对齐限制检查其年龄和因果顺序。反馈无效、超龄或坐标身份不符时，受保护的提案不能进入 UPDATE。Foreground 返回的发布状态与 H0 后续的拒绝原因分别读取。持续超差计算采用 H0 最终有效提案中的位置参考和未被改写的机械采样时间。正常位置整形只改变速度请求。

`abs(有效位置参考 − 实测连续机械位置) > threshold_rad` 的第一笔有效采样进入 `PENDING`，以随后原始采样的时间差累计。阈值内（含相等）为 `TRACKING`。重复消费同一采样不会增加时间。间隔超限从新有效样本重新计时，缺参考和反馈无效分别报告 `NO_REFERENCE` 和 `INVALID_SENSOR`。纯模块拒绝倒序和半程歧义并报告 `TIME_ORDER`，Runtime 的已有 H1 顺序门禁在此类输入到达 H0 前返回 `FOC_RUNTIME_ERR_ORDER`。UInt32 正常回绕继续使用无符号差值。停止、切换模式或进入新 RUNNING 会话时重置计时。

持续时间达到门槛且原 Safety 门禁准许本拍正常提交时，Runtime 在既有提交 Guard 拒绝 UPDATE，Pipeline 提交 INHIBIT、撤销旧 Session，并要求显式重新 ARM/START。独立 STOP 和已有 Safety 故障仍优先。触发原因 `FOLLOWING` 和状态 `TRIGGERED` 可以与 Safety 故障位分别查询。适配器拒绝 INHIBIT 时，`output_confirmed` 为 false 且 `pipeline_status` 保留原拒绝。必须从同一 Runtime 快照读取动作、确认和故障，确认范围以适配器为准。

## Control Plane 和验证入口

使用 [同一份参数表](../../control_plane/README.md) 读取 9 个静态配置及 2 个新增诊断。旧参数和动作适用 [SOFT-01 准入](quickstart.md)。以 独立候选提交相关字段，活动实例暂存仍受 DISABLED、当前 Session、无故障及已确认 INHIBIT 约束，应用方用有效候选显式初始化实例。

[独立 Host 示例](../../examples/motion-protection/README.md) 可以复现向内恢复、向外请求与前馈拒绝、超差停止以及高速惯性越界。[SOFT-02 验收记录](../evidence/2026-11-18-next-soft-02.md) 收录测试、ARM 静态测量和输出确认边界。物理停止距离由机械动能、负载、执行器能力、齿隙、采样与输出链决定。
