# 探针文件，勿合并

本文件只存在于探针 PR（`probe/required-checks-behind`），用来验证规则集 `master-gate` 的 strict 模式：master 前进后，本 PR 应被判为落后并拒绝合并。验证完随 PR 关闭，不进入主干。
