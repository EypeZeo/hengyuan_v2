"""研究侧点时数据合同（PLAN-02 / 任务 3.1、3.4 的数据时间字段基础切片）。

本包承载研究与执行规划（docs/RESEARCH_EXECUTION_PLAN.md）中逐时点数据合同的第一块：
每条记录的时间信息（event_time / available_at / received_at / revision_id / source_id）、
决策读数门禁与 bar 网格缺口识别。后续的实验登记（PLAN-01）、三个账本（PLAN-03）与
统计方法（PLAN-05）模块落在同一包内。

本包只做纯逻辑：不访问仓库分区文件、不做网络 I/O、不触碰任何交易路径。
"""
