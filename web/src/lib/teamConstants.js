// 团队功能的共享常量。与后端口径对齐:
//   - 默认每日额度 30M LLM tokens(src/domain/team/team_meta.hpp)
//   - 在线判定窗口 120 秒(src/host/team_host/team_service.hpp)
export const DEFAULT_DAILY_TOKEN_QUOTA = 30_000_000;
export const MEMBER_STALE_SECONDS = 120;
