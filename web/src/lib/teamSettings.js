// 团队设置页的纯逻辑(可单测,openspec add-decentralized-team tasks 6.2/6.3)。
// 组件层只做渲染,状态合并 / 事件处理 / 展示格式都收敛在这里。

import { ApiError } from './api.js';
import { DEFAULT_DAILY_TOKEN_QUOTA } from './teamConstants.js';

// ApiError code → 中文文案(创建/加入弹框与设置页共用)。
export function teamErrorText(e) {
  if (e instanceof ApiError) {
    const code = e.body?.error || '';
    const mapped = {
      not_admin: '仅管理员可执行该操作',
      invite_invalid: '邀请码无效或已过期',
      team_full: '团队成员已满(50 人)',
      repo_not_linked: '当前项目未关联到该团队',
      too_many_concurrent_sessions: '团队并发会话已达上限(5)',
      quota_exhausted: '目标设备当日额度已用尽',
      member_offline: '目标设备不在线',
      session_not_running: '会话未在运行',
      not_host: '会话不在该设备上',
      team_id_mismatch: '对端返回的团队不一致',
      self_not_in_team: '快照中没有本机身份',
      connection_failed: '连接失败,请检查网络',
      no_lan_address: '未发现可用的局域网地址',
      no_daemon_port: 'daemon 端口未知',
      invalid_name: '请填写团队名称',
      invalid_cwd: '请填写工作目录',
      invalid_link: '请粘贴有效的邀请链接',
      invalid_repo_url: '仓库 URL 无效:请填写可跨设备复用的远程仓库地址',
      workdir_not_git_repo: '工作目录不是 git 仓库,请填写远程仓库 URL',
      repo_mismatch: '工作目录与团队仓库不匹配:请检出团队仓库后重试',
      already_archived: '团队已在归档中',
      already_left: '你已退出该团队',
      not_member: '你已不在该团队中',
      team_not_archived: '只能删除已归档的团队,请先退出团队',
      team_archive_failed: '归档团队失败,请重试',
      session_delete_failed: '删除团队会话失败,请重试',
      team_delete_failed: '删除团队失败,请重试',
    };
    return mapped[code] || e.message || '请求失败,请重试';
  }
  return e?.message || '请求失败,请重试';
}

// WS 消息解析:返回 { type, ... } 对象;非法消息返回 null。
export function parseTeamSocketMessage(data) {
  if (typeof data !== 'string' || data.length > 4 * 1024 * 1024) return null;
  try {
    const parsed = JSON.parse(data);
    return parsed && typeof parsed === 'object' && typeof parsed.type === 'string' ? parsed : null;
  } catch {
    return null;
  }
}

// 应用会话事件流(type=session_event)。
export function applySessionEvent(state, message) {
  if (!message || message.type !== 'session_event' || !message.team_id || !message.session_id) return state;
  return applySessionEventStream(state, message.team_id, message.session_id, {
    seq: message.seq,
    event_kind: message.event_kind,
    data: message.data,
    timestamp_ms: message.timestamp_ms,
  });
}

// 展开状态切换(纯函数)。
export function toggleStreamExpanded(state, sessionId) {
  return {
    ...state,
    expanded: { ...state.expanded, [sessionId]: !state.expanded[sessionId] },
  };
}

// 初始状态:还没有任何团队。
export function initialTeamSettingsState() {
  return {
    teams: [],
    activeTeamId: '',
    streams: {}, // teamId -> sessionId -> 最近事件列表
    expanded: {}, // sessionId -> true(展开查看实时动态)
    error: '',
    wsStatus: 'idle', // idle | connecting | open | closed
  };
}

// 合并一次 team_state 快照(type=team_state)。state 列表整体替换,
// 保持 activeTeamId 指向仍存在的团队。
export function applyTeamState(state, teamId, snapshot) {
  if (!teamId || !snapshot || typeof snapshot !== 'object') return state;
  const incoming = { ...snapshot, team_id: teamId };
  const teams = state.teams.some((team) => team.team_id === teamId)
    ? state.teams.map((team) => (team.team_id === teamId ? incoming : team))
    : [...state.teams, incoming];
  const activeTeamId = state.activeTeamId && teams.some((team) => team.team_id === state.activeTeamId)
    ? state.activeTeamId
    : (teams[0]?.team_id ?? '');
  return { ...state, teams, activeTeamId };
}

// 合并一条会话事件(type=team_session_event)。按 session_id 增删改
// 团队的 sessions 列表;removed 不由后端发出,ended 事件保留记录(状态为准)。
export function applyTeamSessionEvent(state, teamId, event) {
  if (!teamId || !event || !event.session_id) return state;
  const kind = event.kind;
  if (!['created', 'paused', 'resumed', 'host_changed', 'ended'].includes(kind)) return state;
  return {
    ...state,
    teams: state.teams.map((team) => {
      if (team.team_id !== teamId) return team;
      const sessions = Array.isArray(team.sessions) ? [...team.sessions] : [];
      const index = sessions.findIndex((ref) => ref.session_id === event.session_id);
      if (index >= 0) {
        sessions[index] = { ...sessions[index], ...event };
      } else {
        sessions.push({ ...event });
      }
      return { ...team, sessions };
    }),
  };
}

// 合并 WS 消息,返回新状态(不认识的消息原样返回)。
export function applyTeamSocketMessage(state, message) {
  switch (message?.type) {
    case 'team_state':
      return applyTeamState(state, message.team_id, message.state);
    case 'team_session_event':
      return applyTeamSessionEvent(state, message.team_id, message.event);
    default:
      return state;
  }
}

// 跨设备实时查看(3.3):host 会话事件流的只读投影。
// 每会话保留最近 SESSION_STREAM_LIMIT 条,只存展示所需的最小字段。
export const SESSION_STREAM_LIMIT = 30;

export function applySessionEventStream(state, teamId, sessionId, event) {
  if (!teamId || !sessionId || !event || typeof event.seq !== 'number') return state;
  const entry = {
    seq: event.seq,
    kind: event.event_kind || 'unknown',
    text: streamEventText(event.event_kind, event.data),
    timestamp_ms: event.timestamp_ms || 0,
  };
  const teamStreams = state.streams?.[teamId] || {};
  const list = teamStreams[sessionId] || [];
  // 断线重连后 seq 回退:整流重建(旧实例 seq 不可比)。
  const next = list.length && entry.seq <= list[list.length - 1].seq ? [entry] : [...list, entry];
  return {
    ...state,
    streams: { ...state.streams, [teamId]: { ...teamStreams, [sessionId]: next.slice(-SESSION_STREAM_LIMIT) } },
  };
}

// 事件转一行展示文本(message/token 有内容,其余只显示类别)。
export function streamEventText(kind, data) {
  const text = typeof data?.text === 'string' ? data.text : '';
  if (kind === 'message' && text) return text;
  if (kind === 'token' && text) return text;
  const labels = {
    message: '消息', token: '', agent_progress: '', tool_start: '工具执行', tool_end: '工具完成',
    done: '回合完成', busy_changed: '', error: '错误',
  };
  return labels[kind] ?? '';
}

// 展示用:过滤空行,只保留有内容的事件。
export function visibleStreamLines(events) {
  return (events || []).filter((entry) => entry.text);
}

// 会话状态展示:running / paused(带原因) / ended。
export function sessionStateView(ref) {
  const state = ref?.state || 'running';
  if (state === 'paused') return { key: 'paused', label: `已暂停${ref?.pause_reason ? ` · ${ref.pause_reason}` : ''}`, tone: 'warn' };
  if (state === 'ended') return { key: 'ended', label: '已结束', tone: 'mute' };
  return { key: 'running', label: '进行中', tone: 'ok' };
}

// 成员展示名:显示名 > 账户名 > GUID。
export function memberDisplayName(member) {
  return member?.display_name || member?.account || member?.guid || '';
}

// 成员角色展示。
export function memberRoleLabel(member) {
  return member?.role === 'admin' ? '管理员' : '成员';
}

// 每日额度文本:0 = 当日不承接;未设置按默认。
export function quotaText(quota) {
  const value = typeof quota === 'number' ? quota : DEFAULT_DAILY_TOKEN_QUOTA;
  if (value === 0) return '0(当日不承接)';
  return `${(value / 1_000_000).toFixed(0).replace(/\.0$/, '')}M tokens / 日`;
}

// 已用文本。
export function quotaUsedText(used) {
  if (!used) return '0';
  return `${(used / 1_000_000).toFixed(1).replace(/\.0$/, '')}M`;
}

// 邀请码剩余时间文本(expires_at 为 Unix 秒;now 为秒)。
export function inviteRemainingText(expiresAt, now) {
  if (typeof expiresAt !== 'number' || typeof now !== 'number') return '';
  const remaining = Math.floor(expiresAt - now);
  if (remaining <= 0) return '已过期';
  if (remaining >= 60) return `${Math.floor(remaining / 60)} 分 ${remaining % 60} 秒`;
  return `${remaining} 秒`;
}

// 手动接管目标资格(与后端 check_takeover_target 同口径):
// 在线(最后心跳 ≤ 120s)+ 有剩余额度;返回 null 表示可用。
export function takeoverTargetError(member, nowSeconds) {
  if (!member?.guid) return 'member_not_found';
  if (typeof member.last_seen === 'number' && nowSeconds - member.last_seen > 120) return 'member_offline';
  const quota = typeof member.daily_token_quota === 'number' ? member.daily_token_quota : DEFAULT_DAILY_TOKEN_QUOTA;
  const used = member.quota_day != null && member.quota_used_today ? member.quota_used_today : 0;
  if (used >= quota) return 'quota_exhausted';
  return null;
}
