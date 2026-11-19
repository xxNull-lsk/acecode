// 团队(设置 > 集成 > 团队,openspec add-decentralized-team tasks 6.2/6.3)。
// 管理页:创建团队、一次性邀请码(带剩余时间)、成员列表 + 角色管理、
// 每日额度即时保存(无保存按钮);会话列表:发起、结束、手动接管、队员消息
// 入口、暂停/切换事件通知。状态经 /ws/team/<id> 实时合并,纯逻辑在
// lib/teamSettings.js。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api, ApiError } from '../lib/api.js';
import { clsx } from '../lib/format.js';
import {
  applySessionEvent,
  applyTeamSocketMessage,
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
} from '../lib/teamSettings.js';
import { DEFAULT_DAILY_TOKEN_QUOTA } from '../lib/teamConstants.js';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';

const NOW_MS_INTERVAL = 1000;
const QUOTA_MILLION = 1_000_000;

function errText(e) {
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
    };
    return mapped[code] || e.message || '请求失败,请重试';
  }
  return e?.message || '请求失败,请重试';
}

// 状态圆点。
function Dot({ tone = 'ok', label }) {
  return (
    <span className={clsx('flex items-center gap-1.5 text-[12px]', {
      'text-ok': tone === 'ok', 'text-warn': tone === 'warn', 'text-fg-mute': tone === 'mute', 'text-danger': tone === 'danger',
    })}>
      <span className={clsx('w-2 h-2 rounded-full', {
        'bg-ok': tone === 'ok', 'bg-warn': tone === 'warn', 'bg-fg-mute': tone === 'mute', 'bg-danger': tone === 'danger',
      })} />
      {label}
    </span>
  );
}

export function TeamSettings() {
  const [state, setState] = useState(initialTeamSettingsState);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const busyRef = useRef('');
  const [newTeamName, setNewTeamName] = useState('');
  const [invite, setInvite] = useState(null); // { link, expires_at, team_id }
  const [nowSeconds, setNowSeconds] = useState(() => Math.floor(Date.now() / 1000));
  const [quotaDraft, setQuotaDraft] = useState(null); // 字符串输入草稿(仅自己)
  const [takeoverTarget, setTakeoverTarget] = useState(null); // { teamId, sessionId }
  const [notice, setNotice] = useState(null); // { text, tone }
  const wsRef = useRef(null);

  const activeTeam = useMemo(
    () => state.teams.find((team) => team.team_id === state.activeTeamId) || null,
    [state.teams, state.activeTeamId],
  );

  const run = useCallback(async (key, action, { onError } = {}) => {
    if (busyRef.current) return null;
    busyRef.current = key;
    setBusy(key);
    try {
      return await action();
    } catch (e) {
      const message = errText(e);
      if (onError) onError(message);
      else toast({ kind: 'err', text: message });
      return null;
    } finally {
      busyRef.current = '';
      setBusy('');
    }
  }, []);

  const load = useCallback(async () => {
    try {
      const result = await api.listTeams();
      setState((prev) => ({
        ...prev,
        teams: Array.isArray(result?.teams) ? result.teams : [],
        activeTeamId: prev.activeTeamId && result?.teams?.some((t) => t.team_id === prev.activeTeamId)
          ? prev.activeTeamId
          : (result?.teams?.[0]?.team_id ?? ''),
        error: '',
      }));
    } catch (e) {
      setState((prev) => ({ ...prev, error: errText(e) }));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { load(); }, [load]);

  useEffect(() => {
    const timer = setInterval(() => setNowSeconds(Math.floor(Date.now() / 1000)), NOW_MS_INTERVAL);
    return () => clearInterval(timer);
  }, []);

  // 团队 WS:连到当前团队,team_state / team_session_event / session_event 实时合并。
  useEffect(() => {
    if (!activeTeam) return undefined;
    const teamId = activeTeam.team_id;
    setState((prev) => ({ ...prev, wsStatus: 'connecting' }));
    let closed = false;
    let socket = null;
    let retryTimer = null;
    const connect = () => {
      if (closed) return;
      socket = api.teamSocket(teamId);
      wsRef.current = socket;
      socket.onopen = () => setState((prev) => ({ ...prev, wsStatus: 'open' }));
      socket.onmessage = (event) => {
        const message = parseTeamSocketMessage(event.data);
        if (!message) return;
        setState((prev) => {
          let next = applyTeamSocketMessage(prev, message);
          // 跨设备实时查看(3.3):host 会话事件流的只读投影。
          if (message.type === 'session_event') next = applySessionEvent(next, message);
          return next;
        });
        if (message.type === 'team_session_event' && message.team_id === teamId) {
          const changed = message.event || {};
          if (changed.kind === 'paused') {
            const reason = changed.pause_reason || '未说明';
            setNotice({ tone: 'warn', text: `会话「${changed.title || changed.session_id}」已暂停 · ${reason}` });
            toast({ kind: 'warn', text: `团队会话已暂停 · ${reason}` });
          } else if (changed.kind === 'host_changed') {
            setNotice({ tone: 'ok', text: `会话「${changed.title || changed.session_id}」已切换到新设备` });
          }
        }
      };
      socket.onclose = () => {
        setState((prev) => ({ ...prev, wsStatus: 'closed' }));
        // 连接丢失自动重连(3 秒退避);组件卸载/换团队后停止。
        if (!closed) retryTimer = setTimeout(connect, 3000);
      };
      socket.onerror = () => socket.close();
    };
    connect();
    return () => {
      closed = true;
      if (retryTimer) clearTimeout(retryTimer);
      socket?.close();
      wsRef.current = null;
      setState((prev) => ({ ...prev, wsStatus: 'idle' }));
    };
  }, [activeTeam?.team_id]); // eslint-disable-line react-hooks/exhaustive-deps

  // 会话事件的提示(暂停原因 / host 切换)在 WS onmessage 回调里发。
  // 这里不重复监听 connection 全局事件:团队状态走专用 /ws/team 连接。

  const createTeam = () => run('create', async () => {
    const meta = await api.createTeam(newTeamName.trim());
    setNewTeamName('');
    await load();
    setState((prev) => ({ ...prev, activeTeamId: meta.team_id }));
    toast({ kind: 'ok', text: '团队已创建' });
    return meta;
  });

  const issueInvite = () => {
    if (!activeTeam) return;
    run('invite', async () => {
      const result = await api.issueTeamInvite(activeTeam.team_id);
      setInvite({ ...result, team_id: activeTeam.team_id, issued_at: nowSeconds });
      toast({ kind: 'ok', text: '邀请码已生成,15 分钟内有效且只能使用一次' });
      return result;
    });
  };

  const joinTeam = () => {
    const link = window.prompt('粘贴 acecode://team/ 邀请链接');
    if (!link) return;
    run('join', async () => {
      await api.joinTeam(link.trim());
      await load();
      toast({ kind: 'ok', text: '已加入团队' });
    });
  };

  const copyInvite = async () => {
    if (!invite?.link) return;
    try {
      await navigator.clipboard.writeText(invite.link);
      toast({ kind: 'ok', text: '邀请链接已复制' });
    } catch {
      toast({ kind: 'err', text: '复制失败,请手动选择文本' });
    }
  };

  const setRole = (member, role) => {
    if (!activeTeam) return;
    run(`role-${member.guid}`, async () => {
      await api.setTeamRole(activeTeam.team_id, member.guid, role);
      toast({ kind: 'ok', text: `${memberDisplayName(member)} 已设为${memberRoleLabel({ role })}` });
    });
  };

  const saveQuota = () => {
    if (!activeTeam || quotaDraft == null) return;
    const value = Math.floor(Number(quotaDraft) * QUOTA_MILLION);
    if (!Number.isFinite(value) || value < 0) {
      toast({ kind: 'err', text: '额度无效:请输入不小于 0 的百万 tokens 数,0 表示当日不承接' });
      setQuotaDraft(null);
      return;
    }
    setQuotaDraft(null);
    run('quota', async () => {
      await api.setTeamQuota(activeTeam.team_id, value);
      toast({ kind: 'ok', text: '每日额度已保存' });
    });
  };

  const startSession = () => {
    if (!activeTeam) return;
    run('start-session', async () => {
      // 仓库未关联时先关联本机项目再重试一次。
      const start = () => api.startTeamSession(activeTeam.team_id, { title: newTeamName.trim() || '' });
      let ref;
      try {
        ref = await start();
      } catch (e) {
        if (e instanceof ApiError && e.body?.error === 'repo_not_linked') {
          await api.linkTeamRepo(activeTeam.team_id, '');
          ref = await start();
        } else {
          throw e;
        }
      }
      toast({ kind: 'ok', text: '团队会话已发起' });
      return ref;
    });
  };

  const endSession = (ref) => {
    if (!activeTeam) return;
    run(`end-${ref.session_id}`, async () => {
      await api.setTeamSessionState(activeTeam.team_id, ref.session_id, 'ended');
      toast({ kind: 'ok', text: '会话已结束' });
    });
  };

  const takeover = (target) => {
    const { teamId, sessionId } = takeoverTarget;
    setTakeoverTarget(null);
    run(`takeover-${sessionId}`, async () => {
      // 接管前先关联本机项目(repo 未登记时接管数据落不进团队身份)。
      try {
        await api.linkTeamRepo(teamId, '');
      } catch { /* 关联失败不阻断:接管侧仍校验 */ }
      await api.takeoverTeamSession(teamId, sessionId, '');
      toast({ kind: 'ok', text: `已接管到 ${memberDisplayName(target)}` });
    }, { onError: (message) => toast({ kind: 'err', text: `接管失败:${message}` }) });
  };

  const sendMessage = (ref, text) => {
    if (!activeTeam || !text.trim()) return;
    run(`msg-${ref.session_id}`, async () => {
      await api.sendTeamSessionMessage(activeTeam.team_id, ref.session_id, text.trim());
      toast({ kind: 'ok', text: '消息已发送' });
    });
  };

  const self = activeTeam?.members?.find((member) => member.guid === activeTeam.self?.guid);
  const isAdmin = self?.role === 'admin';

  return (
    <div className="flex-1 overflow-y-auto px-12 py-6">
      <h2 className="text-xl font-bold mb-2">团队</h2>
      <p className="text-[12px] text-fg-mute mb-5">
        与局域网内的设备组成团队,跨设备查看与续写团队会话。邀请码 15 分钟内有效且只能使用一次。
      </p>

      {loading && <div className="text-[13px] text-fg-mute">加载中…</div>}
      {!loading && state.error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
          {state.error}
        </div>
      )}

      {!loading && !activeTeam && (
        <div className="max-w-md">
          <div className="flex items-center gap-2 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
            <input
              value={newTeamName}
              onChange={(e) => setNewTeamName(e.target.value)}
              onKeyDown={(e) => { if (e.key === 'Enter') createTeam(); }}
              placeholder="团队名称"
              className="flex-1 h-7 px-2 text-[13px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
            />
            <button
              type="button"
              disabled={busy === 'create'}
              onClick={createTeam}
              className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
            >创建团队</button>
            <button
              type="button"
              disabled={busy === 'join'}
              onClick={joinTeam}
              className="px-3 py-1 rounded border border-border text-[13px] text-fg hover:bg-surface-hi transition disabled:opacity-60"
            >加入团队</button>
          </div>
        </div>
      )}

      {activeTeam && (
        <>
          <div className="flex items-center justify-between mb-3">
            <div className="flex items-center gap-2 text-[14px] text-fg">
              {activeTeam.name || '团队'}
              {state.teams.length > 1 && (
                <select
                  value={state.activeTeamId}
                  onChange={(e) => setState((prev) => ({ ...prev, activeTeamId: e.target.value }))}
                  aria-label="切换团队"
                  className="h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
                >
                  {state.teams.map((team) => (
                    <option key={team.team_id} value={team.team_id}>{team.name || team.team_id}</option>
                  ))}
                </select>
              )}
            </div>
            <Dot
              tone={state.wsStatus === 'open' ? 'ok' : 'warn'}
              label={state.wsStatus === 'open' ? '已连接' : '连接中…'}
            />
          </div>

          {notice && (
            <div className={clsx('mb-3 px-3 py-2 rounded-md border bg-surface text-[12px] flex items-center justify-between gap-3',
              notice.tone === 'warn' ? 'border-warn text-warn' : 'border-border text-fg')}>
              <span>{notice.text}</span>
              <button type="button" className="text-[11px] text-fg-mute hover:underline" onClick={() => setNotice(null)}>知道了</button>
            </div>
          )}

          {/* 邀请码 */}
          <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
            <div className="min-w-0">
              <div className="text-[13px] font-normal text-fg">邀请队员</div>
              {invite && invite.team_id === activeTeam.team_id && (
                <div className="text-[11px] text-fg-mute mt-0.5 truncate max-w-[26rem]" title={invite.link}>
                  {invite.link}
                </div>
              )}
            </div>
            <div className="flex items-center gap-2 shrink-0">
              {invite && invite.team_id === activeTeam.team_id && (
                <>
                  <Dot tone={nowSeconds < invite.expires_at ? 'ok' : 'danger'}
                       label={inviteRemainingText(invite.expires_at, nowSeconds)} />
                  <button type="button" className="px-1.5 py-0.5 text-[11px] hover:underline" onClick={copyInvite}>复制</button>
                </>
              )}
              {isAdmin && (
                <button
                  type="button"
                  disabled={busy === 'invite'}
                  onClick={issueInvite}
                  className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
                >生成邀请码</button>
              )}
            </div>
          </div>

          {/* 成员列表 */}
          <div className="text-[14px] font-semibold mt-4 mb-2">成员</div>
          {(activeTeam.members || []).map((member) => {
            const isSelf = member.guid === activeTeam.self?.guid;
            const online = typeof member.last_seen === 'number' && nowSeconds - member.last_seen <= 120;
            const error = takeoverTargetError(member, nowSeconds);
            return (
              <div key={member.guid} className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
                <div>
                  <div className="text-[13px] font-normal text-fg">
                    {memberDisplayName(member)}
                    {isSelf && <span className="text-fg-mute"> (本机)</span>}
                  </div>
                  <div className="text-[11px] text-fg-mute mt-0.5">
                    {memberRoleLabel(member)} · {quotaText(member.daily_token_quota)} · 已用 {quotaUsedText(member.quota_used_today)}
                  </div>
                </div>
                <div className="flex items-center gap-2 shrink-0">
                  <Dot tone={online ? 'ok' : 'mute'} label={online ? '在线' : '离线'} />
                  {isSelf ? (
                    <input
                      type="number"
                      min="0"
                      value={quotaDraft ?? (member.daily_token_quota ?? DEFAULT_DAILY_TOKEN_QUOTA) / QUOTA_MILLION}
                      onChange={(e) => setQuotaDraft(e.target.value)}
                      onBlur={saveQuota}
                      onKeyDown={(e) => { if (e.key === 'Enter') saveQuota(); }}
                      aria-label="本机每日额度(百万 tokens)"
                      className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
                    />
                  ) : isAdmin && (
                    <button
                      type="button"
                      className="px-1.5 py-0.5 text-[11px] hover:underline"
                      disabled={busy === `role-${member.guid}`}
                      onClick={() => setRole(member, member.role === 'admin' ? 'member' : 'admin')}
                    >
                      {member.role === 'admin' ? '取消管理员' : '设为管理员'}
                    </button>
                  )}
                  {!isSelf && error === null && (
                    <span className="text-[11px] text-ok" title="可作为接管目标">可接管</span>
                  )}
                </div>
              </div>
            );
          })}

          {/* 会话列表 */}
          <div className="text-[14px] font-semibold mt-5 mb-2">团队会话</div>
          {(activeTeam.sessions || []).length === 0 && (
            <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 text-[13px] text-fg-mute">
              还没有团队会话。
            </div>
          )}
          {(activeTeam.sessions || []).map((ref) => {
            const view = sessionStateView(ref);
            const hostMember = (activeTeam.members || []).find((member) => member.guid === ref.host_guid);
            const canSend = view.key === 'running';
            const expanded = Boolean(state.expanded[ref.session_id]);
            const lines = expanded
              ? visibleStreamLines(state.streams?.[activeTeam.team_id]?.[ref.session_id])
              : [];
            return (
              <div key={ref.session_id} className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
                <div className="flex items-center justify-between gap-3">
                  <div className="min-w-0">
                    <div className="text-[13px] font-normal text-fg truncate">
                      {ref.title || ref.session_id}
                    </div>
                    <div className="text-[11px] text-fg-mute mt-0.5">
                      host:{' '}{hostMember ? memberDisplayName(hostMember) : (ref.host_guid || '未知')}
                      {ref.host_guid === activeTeam.self?.guid ? '(本机)' : ''}
                    </div>
                  </div>
                  <div className="flex items-center gap-2 shrink-0">
                    <Dot tone={view.tone} label={view.label} />
                    {view.key !== 'ended' && (
                      <>
                        <button
                          type="button"
                          aria-expanded={expanded}
                          className="px-1.5 py-0.5 text-[11px] hover:underline"
                          onClick={() => setState((prev) => toggleStreamExpanded(prev, ref.session_id))}
                        >{expanded ? '收起动态' : '实时动态'}</button>
                        <button
                          type="button"
                          className="px-1.5 py-0.5 text-[11px] hover:underline"
                          onClick={() => {
                            const text = window.prompt('发给团队会话的消息');
                            if (text) sendMessage(ref, text);
                          }}
                        >发消息</button>
                        <button
                          type="button"
                          className="px-1.5 py-0.5 text-[11px] hover:underline"
                          disabled={busy === `takeover-${ref.session_id}`}
                          onClick={() => setTakeoverTarget({ teamId: activeTeam.team_id, sessionId: ref.session_id, ref })}
                        >切换到本机</button>
                        <button
                          type="button"
                          className="px-1.5 py-0.5 text-[11px] text-danger hover:underline"
                          disabled={busy === `end-${ref.session_id}`}
                          onClick={() => endSession(ref)}
                        >结束</button>
                      </>
                    )}
                  </div>
                </div>
                {expanded && (
                  <div className="mt-2 rounded-md bg-surface-alt border border-border px-3 py-2 max-h-40 overflow-y-auto ace-scrollbar">
                    {lines.length === 0 && <div className="text-[11px] text-fg-mute">暂无动态</div>}
                    {lines.map((entry) => (
                      <div key={entry.seq} className="text-[12px] text-fg whitespace-pre-wrap break-words">
                        {entry.text}
                      </div>
                    ))}
                  </div>
                )}
              </div>
            );
          })}
          <button
            type="button"
            disabled={busy === 'start-session'}
            onClick={startSession}
            className="mt-1 px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
          >发起团队会话</button>

          <div className="h-px bg-border my-5" />
          <div className="text-[12px] text-fg-mute">加入其他团队:</div>
          <button
            type="button"
            disabled={busy === 'join'}
            onClick={joinTeam}
            className="mt-2 px-3 py-1 rounded border border-border text-[13px] text-fg hover:bg-surface-hi transition disabled:opacity-60"
          >粘贴邀请链接加入</button>
        </>
      )}

      {/* 手动接管目标选择:在线 + 有剩余额度的队员 */}
      {takeoverTarget && (
        <Modal onClose={() => setTakeoverTarget(null)} dismissOnBackdrop={false} labelledBy="team-takeover-title">
          <div className="min-w-[20rem]">
            <h3 id="team-takeover-title" className="text-[14px] font-bold mb-3">选择接管目标设备</h3>
            {(activeTeam?.members || []).filter((member) => member.guid !== activeTeam.self?.guid).map((member) => {
              const error = takeoverTargetError(member, nowSeconds);
              return (
                <div key={member.guid} className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
                  <div>
                    <div className="text-[13px] font-normal text-fg">{memberDisplayName(member)}</div>
                    <div className="text-[11px] text-fg-mute mt-0.5">
                      {error === 'member_offline' ? '离线' : error === 'quota_exhausted' ? '当日额度已用尽' : '可接管'}
                    </div>
                  </div>
                  <button
                    type="button"
                    disabled={Boolean(error) || busy === `takeover-${takeoverTarget.sessionId}`}
                    onClick={() => takeover(member)}
                    className="px-3 py-1 bg-accent text-white rounded disabled:opacity-40"
                  >接管</button>
                </div>
              );
            })}
          </div>
        </Modal>
      )}
    </div>
  );
}
