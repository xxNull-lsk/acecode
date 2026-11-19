// 团队侧边栏投影(add-team-workdir-sidebar tasks 3.2)。
// GET /api/team 的每个团队状态 → 侧边栏「团队」分组的纯数据投影:
// 会话行直接消费 daemon 的本机定位投影(cwd/workspace_hash 只在本机
// 会话上出现),本机存在的会话可当普通会话打开,远程托管的会话只能
// 走接管入口。与设置页共用 sessionStateView 的状态口径。

import { sessionStateView } from './teamSettings.js';

// 状态视图口径与设置页一致,别名导出避免侧边栏依赖设置页命名。
export function teamSessionStateView(ref) {
  return sessionStateView(ref);
}

export { sessionStateView };

// 折叠态可见条数(与工作区分组的侧栏折叠口径一致)。
export const TEAM_SESSION_COLLAPSE_LIMIT = 5;

// TeamSessionRef 公开视图 → 侧边栏会话行。daemon 在本机存在该会话数据
// 时才下发 cwd/workspace_hash(定位投影),据此判定可打开性;no_workspace
// 会话只有 cwd,走无工作区打开路径。
export function teamSidebarSessionRow(ref) {
  if (!ref?.session_id) return null;
  const cwd = ref.cwd || '';
  const workspaceHash = ref.workspace_hash || '';
  const isLocal = Boolean(cwd || workspaceHash);
  return {
    id: ref.session_id,
    title: ref.title || ref.session_id,
    state: ref.state || 'running',
    pauseReason: ref.pause_reason || '',
    hostGuid: ref.host_guid || '',
    createdBy: ref.created_by || '',
    updatedAt: ref.updated_at || 0,
    isLocal,
    cwd,
    workspaceHash,
    noWorkspace: isLocal && !workspaceHash,
  };
}

// 团队状态 → 会话行列表(保持 daemon 顺序:session_refs 本身按时间维护)。
export function teamSidebarSessionRows(team) {
  return (team?.sessions || [])
    .map(teamSidebarSessionRow)
    .filter(Boolean);
}

// 折叠分页:visibleLimit 语义与工作区分组一致(折叠 5 条,展开后翻页)。
// 返回当前可见行、总数与「展开/收起」动作(空 = 不出按钮)。
export function teamSessionListProjection(rows, visibleLimit) {
  const limit = Number.isFinite(visibleLimit) && visibleLimit >= TEAM_SESSION_COLLAPSE_LIMIT
    ? visibleLimit
    : TEAM_SESSION_COLLAPSE_LIMIT;
  const total = rows.length;
  if (total <= TEAM_SESSION_COLLAPSE_LIMIT) {
    return { visibleRows: rows, total, action: '' };
  }
  const visibleRows = rows.slice(0, limit);
  return {
    visibleRows,
    total,
    action: total > limit ? 'expand' : (limit > TEAM_SESSION_COLLAPSE_LIMIT ? 'collapse' : ''),
  };
}

// 打开目标:本机会话 → 交给 selectSession 的定位信息;远程托管 → null
// (侧边栏不直接打开,提供接管入口)。
export function teamSessionOpenTarget(row) {
  if (!row?.isLocal) return null;
  if (row.noWorkspace || !row.workspaceHash) {
    return { noWorkspace: true, workspaceHash: '', cwd: row.cwd || '' };
  }
  return { noWorkspace: false, workspaceHash: row.workspaceHash, cwd: row.cwd || '' };
}

// 团队行归一化(分组头用):id、名称、会话数。
export function teamSidebarTeam(team) {
  if (!team?.team_id) return null;
  return {
    teamId: team.team_id,
    name: team.name || team.team_id,
    sessionCount: (team.sessions || []).length,
  };
}
