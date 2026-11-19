// 覆盖 web/src/lib/teamSettings.js 的纯逻辑(6.2/6.3):
// WS 消息解析与合并、会话状态视图、资格校验、展示格式。

import assert from 'node:assert/strict';
import {
  applySessionEvent,
  applyTeamSessionEvent,
  applyTeamSocketMessage,
  applyTeamState,
  initialTeamSettingsState,
  inviteRemainingText,
  memberDisplayName,
  memberRoleLabel,
  parseTeamSocketMessage,
  quotaText,
  quotaUsedText,
  sessionStateView,
  streamEventText,
  takeoverTargetError,
  toggleStreamExpanded,
  visibleStreamLines,
} from './teamSettings.js';

const SNAPSHOT = {
  name: '研发组',
  members: [
    { guid: 'guid-a', account: 'alice@pc-a', role: 'admin', last_seen: 100, daily_token_quota: 30_000_000 },
  ],
  sessions: [],
  self: { guid: 'guid-a', quota_exhausted: false },
};

// 场景: 非法 WS 消息拒绝,合法消息解析
{
  assert.equal(parseTeamSocketMessage('not json'), null);
  assert.equal(parseTeamSocketMessage('123'), null);
  assert.equal(parseTeamSocketMessage('{"no_type":1}'), null);
  const parsed = parseTeamSocketMessage('{"type":"team_state","team_id":"t1"}');
  assert.equal(parsed.type, 'team_state');
  assert.equal(parsed.team_id, 't1');
}

// 场景: team_state 快照合并 — 新团队追加、已有团队整体替换、activeTeamId 保持
{
  let state = initialTeamSettingsState();
  state = applyTeamSocketMessage(state, { type: 'team_state', team_id: 't1', state: SNAPSHOT });
  assert.equal(state.teams.length, 1);
  assert.equal(state.activeTeamId, 't1');
  state = applyTeamSocketMessage(state, { type: 'team_state', team_id: 't1', state: { ...SNAPSHOT, name: '新名' } });
  assert.equal(state.teams.length, 1);
  assert.equal(state.teams[0].name, '新名');
  state = applyTeamSocketMessage(state, { type: 'team_state', team_id: 't2', state: { ...SNAPSHOT, name: '二队' } });
  assert.equal(state.teams.length, 2);
  assert.equal(state.activeTeamId, 't1');
  // 非法消息不改状态
  assert.equal(applyTeamSocketMessage(state, { type: 'unknown' }), state);
}

// 场景: 会话事件按 session_id 增改;created 新增,host_changed 更新,ended 保留
{
  let state = applyTeamState(initialTeamSettingsState(), 't1', SNAPSHOT);
  state = applyTeamSessionEvent(state, 't1', { kind: 'created', session_id: 's-1', title: '调试', host_guid: 'guid-a', state: 'running' });
  assert.equal(state.teams[0].sessions.length, 1);
  assert.equal(state.teams[0].sessions[0].title, '调试');
  state = applyTeamSessionEvent(state, 't1', { kind: 'host_changed', session_id: 's-1', host_guid: 'guid-b', state: 'running' });
  assert.equal(state.teams[0].sessions[0].host_guid, 'guid-b');
  // 标题不被事件里的空字段覆盖(对象展开只覆盖事件带上的字段)
  assert.equal(state.teams[0].sessions[0].title, '调试');
  state = applyTeamSessionEvent(state, 't1', { kind: 'ended', session_id: 's-1', state: 'ended' });
  assert.equal(state.teams[0].sessions[0].state, 'ended');
  assert.equal(state.teams[0].sessions.length, 1);
  // 未支持的事件类型忽略
  assert.equal(applyTeamSessionEvent(state, 't1', { kind: 'removed', session_id: 's-9' }).teams[0].sessions.length, 1);
  // 其他团队的会话不影响本团队
  assert.equal(applyTeamSessionEvent(state, 't2', { kind: 'created', session_id: 's-9' }).teams[0].sessions.length, 1);
}

// 场景: 会话状态视图(暂停带原因)
{
  assert.deepEqual(sessionStateView({ state: 'running' }), { key: 'running', label: '进行中', tone: 'ok' });
  const paused = sessionStateView({ state: 'paused', pause_reason: '失联' });
  assert.equal(paused.tone, 'warn');
  assert.match(paused.label, /失联/);
  assert.equal(sessionStateView({ state: 'ended' }).tone, 'mute');
}

// 场景: 成员展示与额度格式
{
  assert.equal(memberDisplayName({ guid: 'g', account: 'a@pc', display_name: '小明' }), '小明');
  assert.equal(memberDisplayName({ guid: 'g', account: 'a@pc' }), 'a@pc');
  assert.equal(memberRoleLabel({ role: 'admin' }), '管理员');
  assert.equal(memberRoleLabel({ role: 'member' }), '成员');
  assert.match(quotaText(30_000_000), /30M/);
  assert.match(quotaText(0), /不承接/);
  assert.equal(quotaUsedText(0), '0');
  assert.match(quotaUsedText(2_500_000), /2.5M/);
}

// 场景: 邀请码倒计时
{
  assert.match(inviteRemainingText(1_000 + 125, 1_000), /2 分 5 秒/);
  assert.equal(inviteRemainingText(1_000 + 45, 1_000), '45 秒');
  assert.equal(inviteRemainingText(1_000, 1_001), '已过期');
}

// 场景: 接管目标资格(在线 + 额度),与后端同口径
{
  const now = 1_000;
  assert.equal(takeoverTargetError({ guid: 'b', last_seen: now - 10, daily_token_quota: 30_000_000, quota_used_today: 0, quota_day: 1 }, now), null);
  assert.equal(takeoverTargetError({ guid: 'b', last_seen: now - 121, daily_token_quota: 30_000_000, quota_used_today: 0, quota_day: 1 }, now), 'member_offline');
  assert.equal(takeoverTargetError({ guid: 'b', last_seen: now - 10, daily_token_quota: 0 }, now), 'quota_exhausted');
  assert.equal(takeoverTargetError({ guid: 'b', last_seen: now - 10, daily_token_quota: 10_000_000, quota_used_today: 10_000_000, quota_day: 1 }, now), 'quota_exhausted');
  assert.equal(takeoverTargetError({}, now), 'member_not_found');
}

// 场景: 会话事件流只读投影 — seq 单调追加、断线重连(seq 回退)整流重建、
// 只保留最近 30 条、空文本事件不进展示行
{
  let state = initialTeamSettingsState();
  for (let seq = 1; seq <= 35; ++seq) {
    state = applySessionEvent(state, {
      type: 'session_event', team_id: 't1', session_id: 's-1', seq,
      event_kind: 'message', data: { text: `第 ${seq} 条` },
    });
  }
  const stream = state.streams.t1['s-1'];
  assert.equal(stream.length, 30);
  assert.equal(stream.at(-1).seq, 35);
  assert.equal(stream[0].seq, 6);
  // seq 回退(重连/新实例):整流重建
  state = applySessionEvent(state, { type: 'session_event', team_id: 't1', session_id: 's-1', seq: 1, event_kind: 'message', data: { text: '重建' } });
  assert.equal(state.streams.t1['s-1'].length, 1);
  // 空文本事件可入流但不进展示行
  state = applySessionEvent(state, { type: 'session_event', team_id: 't1', session_id: 's-1', seq: 2, event_kind: 'agent_progress', data: {} });
  assert.equal(state.streams.t1['s-1'].length, 2);
  assert.equal(visibleStreamLines(state.streams.t1['s-1']).length, 1);
  // 展开/收起
  state = toggleStreamExpanded(state, 's-1');
  assert.equal(state.expanded['s-1'], true);
  state = toggleStreamExpanded(state, 's-1');
  assert.equal(state.expanded['s-1'], false);
  // 事件文本映射
  assert.equal(streamEventText('message', { text: '你好' }), '你好');
  assert.equal(streamEventText('tool_start', {}), '工具执行');
  assert.equal(streamEventText('unknown', {}), '');
}

console.log('teamSettings.test.js ok');
