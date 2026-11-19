// 覆盖 web/src/lib/teamSidebar.js 的纯逻辑(tasks 3.2):
// 定位投影 → 可打开性判定、折叠分页、远程会话不直接打开。

import assert from 'node:assert/strict';
import {
  TEAM_SESSION_COLLAPSE_LIMIT,
  teamSessionListProjection,
  teamSessionOpenTarget,
  teamSidebarSessionRow,
  teamSidebarSessionRows,
  teamSidebarTeam,
} from './teamSidebar.js';

const remoteRef = {
  session_id: 's-remote',
  title: '远端会话',
  host_guid: 'guid-b',
  state: 'running',
};

const localRef = {
  session_id: 's-local',
  title: '本机会话',
  host_guid: 'guid-a',
  state: 'running',
  cwd: '/home/a/demo',
  workspace_hash: 'abcd1234efgh5678',
};

const localNoWorkspaceRef = {
  session_id: 's-nw',
  host_guid: 'guid-a',
  state: 'paused',
  pause_reason: '失联',
  cwd: '/tmp/scratch',
};

// 远程会话:稀疏引用,不可直接打开
{
  const row = teamSidebarSessionRow(remoteRef);
  assert.equal(row.isLocal, false);
  assert.equal(row.noWorkspace, false);
  assert.equal(row.cwd, '');
  assert.equal(teamSessionOpenTarget(row), null);
}

// 本机会话:定位投影 → 可按工作区打开
{
  const row = teamSidebarSessionRow(localRef);
  assert.equal(row.isLocal, true);
  assert.equal(row.workspaceHash, 'abcd1234efgh5678');
  assert.deepEqual(teamSessionOpenTarget(row), {
    noWorkspace: false,
    workspaceHash: 'abcd1234efgh5678',
    cwd: '/home/a/demo',
  });
}

// 本机 no_workspace 会话:只有 cwd,走无工作区路径
{
  const row = teamSidebarSessionRow(localNoWorkspaceRef);
  assert.equal(row.isLocal, true);
  assert.equal(row.noWorkspace, true);
  assert.deepEqual(teamSessionOpenTarget(row), {
    noWorkspace: true,
    workspaceHash: '',
    cwd: '/tmp/scratch',
  });
}

// 无效引用返回 null;状态缺省按 running
{
  assert.equal(teamSidebarSessionRow(null), null);
  assert.equal(teamSidebarSessionRow({ session_id: 's1' }).state, 'running');
}

// 列表过滤无效引用
assert.equal(teamSidebarSessionRows({ sessions: [remoteRef, null, localRef] }).length, 2);

// 折叠分页:不超上限不分页
{
  const few = teamSidebarSessionRows({ sessions: Array.from({ length: TEAM_SESSION_COLLAPSE_LIMIT }, (_, i) => ({ ...remoteRef, session_id: `s-${i}` })) });
  const p = teamSessionListProjection(few, undefined);
  assert.equal(p.visibleRows, few);
  assert.equal(p.action, '');
}

// 超上限折叠,标记 expand 动作
{
  const rows = teamSidebarSessionRows({ sessions: Array.from({ length: 7 }, (_, i) => ({ ...remoteRef, session_id: `s-${i}` })) });
  const p = teamSessionListProjection(rows, undefined);
  assert.equal(p.total, 7);
  assert.equal(p.visibleRows.length, TEAM_SESSION_COLLAPSE_LIMIT);
  assert.equal(p.action, 'expand');

  // 展开后未读完继续翻页;读完标记 collapse
  const expanded = teamSessionListProjection(rows, 6);
  assert.equal(expanded.visibleRows.length, 6);
  assert.equal(expanded.action, 'expand');
  const done = teamSessionListProjection(rows, 7);
  assert.equal(done.visibleRows.length, 7);
  assert.equal(done.action, 'collapse');

  // 非法 visibleLimit 回退折叠上限
  assert.equal(teamSessionListProjection(rows, 0).visibleRows.length, TEAM_SESSION_COLLAPSE_LIMIT);
}

// 团队行归一化;无效返回 null
{
  assert.deepEqual(
    teamSidebarTeam({ team_id: 't1', name: '研发组', sessions: [localRef] }),
    { teamId: 't1', name: '研发组', sessionCount: 1 },
  );
  assert.equal(teamSidebarTeam({ team_id: 't1' }).name, 't1');
  assert.equal(teamSidebarTeam(null), null);
  assert.equal(teamSidebarTeam({ sessions: [] }), null); // 无 team_id 返回 null
}
