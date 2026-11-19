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
import { VsIcon } from './Icon.jsx';
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
      invalid_name: '请填写团队名称',
      invalid_cwd: '请填写工作目录',
      invalid_link: '请粘贴有效的邀请链接',
      invalid_repo_url: '仓库 URL 无效:请填写可跨设备复用的远程仓库地址',
      workdir_not_git_repo: '工作目录不是 git 仓库,请填写远程仓库 URL',
      repo_mismatch: '工作目录与团队仓库不匹配:请检出团队仓库后重试',
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
  // 创建/加入弹框表单(add-team-workdir-sidebar):null = 关闭。
  const [createModal, setCreateModal] = useState(null); // { name, cwd, repoUrl, error, busy }
  const [joinModal, setJoinModal] = useState(null); // { link, cwd, error, busy }
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

  // 创建团队:名称 + 工作目录(是 git 仓库时可省略仓库 URL,否则必填,
  // 以后端校验为准)。成功后切到新团队。
  const submitCreate = async () => {
    const form = createModal;
    if (!form || form.busy) return;
    const name = form.name.trim();
    const cwd = form.cwd.trim();
    if (!name) { setCreateModal({ ...form, error: '请填写团队名称' }); return; }
    if (!cwd) { setCreateModal({ ...form, error: '请填写工作目录' }); return; }
    setCreateModal({ ...form, busy: true, error: '' });
    const meta = await run('create', () => api.createTeam(name, { cwd, repoUrl: form.repoUrl.trim() }),
      { onError: (message) => setCreateModal((prev) => (prev ? { ...prev, busy: false, error: message } : prev)) });
    if (!meta) return;
    setCreateModal(null);
    await load();
    window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
    setState((prev) => ({ ...prev, activeTeamId: meta.team_id }));
    toast({ kind: 'ok', text: '团队已创建' });
  };

  const issueInvite = () => {
    if (!activeTeam) return;
    run('invite', async () => {
      const result = await api.issueTeamInvite(activeTeam.team_id);
      setInvite({ ...result, team_id: activeTeam.team_id, issued_at: nowSeconds });
      toast({ kind: 'ok', text: '邀请码已生成,15 分钟内有效且只能使用一次' });
      return result;
    });
  };

  // 加入团队:邀请链接 + 本机工作目录。git 仓库从团队信息自动匹配,
  // 不匹配(目录未检出团队仓库)时后端拒绝并提示原因。
  const submitJoin = async () => {
    const form = joinModal;
    if (!form || form.busy) return;
    const link = form.link.trim();
    const cwd = form.cwd.trim();
    if (!link) { setJoinModal({ ...form, error: '请粘贴邀请链接' }); return; }
    if (!cwd) { setJoinModal({ ...form, error: '请填写工作目录' }); return; }
    setJoinModal({ ...form, busy: true, error: '' });
    const ok = await run('join', () => api.joinTeam(link, cwd),
      { onError: (message) => setJoinModal((prev) => (prev ? { ...prev, busy: false, error: message } : prev)) });
    if (!ok) return;
    setJoinModal(null);
    await load();
    window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
    toast({ kind: 'ok', text: '已加入团队' });
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
      const start = () => api.startTeamSession(activeTeam.team_id, {});
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
            <button
              type="button"
              disabled={busy === 'create'}
              onClick={() => setCreateModal({ name: '', cwd: '', repoUrl: '', error: '', busy: false })}
              className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
            >创建团队</button>
            <button
              type="button"
              disabled={busy === 'join'}
              onClick={() => setJoinModal({ link: '', cwd: '', error: '', busy: false })}
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
            onClick={() => setJoinModal({ link: '', cwd: '', error: '', busy: false })}
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

      {/* 创建团队:名称 + 工作目录(是 git 仓库时可省略仓库 URL) */}
      {createModal && (
        <Modal onClose={() => setCreateModal(null)} width={520} dismissOnBackdrop={false} labelledBy="team-create-title">
          {({ close }) => (
            <form
              onSubmit={(event) => { event.preventDefault(); submitCreate(); }}
              className="ace-modal-form"
            >
              <div className="shrink-0 px-5 py-4 border-b border-border flex items-start gap-3">
                <div className="w-9 h-9 rounded-lg bg-accent/10 text-accent flex items-center justify-center shrink-0">
                  <VsIcon name="antenna" size={19} />
                </div>
                <div className="min-w-0">
                  <h2 id="team-create-title" className="text-[15px] font-semibold text-fg">创建团队</h2>
                  <p className="mt-0.5 text-[12px] leading-5 text-fg-mute">
                    团队围绕一个工作目录与 git 仓库协作。
                  </p>
                </div>
              </div>
              <div className="min-h-0 overflow-y-auto ace-scrollbar px-5 py-4 space-y-4">
                <label className="block">
                  <span className="block mb-1.5 text-[12px] font-medium text-fg">团队名称</span>
                  <input
                    value={createModal.name}
                    onChange={(e) => setCreateModal((prev) => ({ ...prev, name: e.target.value, error: '' }))}
                    maxLength={128}
                    autoComplete="off"
                    spellCheck={false}
                    className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent"
                  />
                </label>
                <label className="block">
                  <span className="block mb-1.5 text-[12px] font-medium text-fg">工作目录</span>
                  <input
                    value={createModal.cwd}
                    onChange={(e) => setCreateModal((prev) => ({ ...prev, cwd: e.target.value, error: '' }))}
                    autoComplete="off"
                    spellCheck={false}
                    placeholder="/home/user/project"
                    className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] font-mono text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
                  />
                </label>
                <label className="block">
                  <span className="block mb-1.5 text-[12px] font-medium text-fg">git 仓库 URL(选填)</span>
                  <input
                    value={createModal.repoUrl}
                    onChange={(e) => setCreateModal((prev) => ({ ...prev, repoUrl: e.target.value, error: '' }))}
                    autoComplete="off"
                    spellCheck={false}
                    placeholder="https://github.com/org/repo.git"
                    className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] font-mono text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
                  />
                  <span className="block mt-1.5 text-[11px] leading-5 text-fg-mute">
                    工作目录是 git 仓库时自动从目录解析;不是 git 仓库时必填。
                  </span>
                </label>
                {createModal.error && (
                  <div role="alert" className="px-3 py-2 rounded-lg border border-danger/30 bg-surface text-[12px] leading-5 text-danger">
                    {createModal.error}
                  </div>
                )}
              </div>
              <div className="shrink-0 px-5 py-3 border-t border-border bg-surface-alt flex items-center justify-end gap-2">
                <button
                  type="button"
                  onClick={close}
                  className="h-8 px-3 rounded-lg text-[12px] text-fg hover:bg-surface-hi"
                >取消</button>
                <button
                  type="submit"
                  data-ace-dialog-primary="true"
                  disabled={createModal.busy}
                  className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 flex items-center justify-center gap-1.5"
                >
                  {createModal.busy && <span className="ace-spinner w-3 h-3" />}
                  {createModal.busy ? '创建中…' : '创建团队'}
                </button>
              </div>
            </form>
          )}
        </Modal>
      )}

      {/* 加入团队:邀请链接 + 工作目录;git 仓库从团队信息自动匹配 */}
      {joinModal && (
        <Modal onClose={() => setJoinModal(null)} width={520} dismissOnBackdrop={false} labelledBy="team-join-title">
          {({ close }) => (
            <form
              onSubmit={(event) => { event.preventDefault(); submitJoin(); }}
              className="ace-modal-form"
            >
              <div className="shrink-0 px-5 py-4 border-b border-border flex items-start gap-3">
                <div className="w-9 h-9 rounded-lg bg-accent/10 text-accent flex items-center justify-center shrink-0">
                  <VsIcon name="antenna" size={19} />
                </div>
                <div className="min-w-0">
                  <h2 id="team-join-title" className="text-[15px] font-semibold text-fg">加入团队</h2>
                  <p className="mt-0.5 text-[12px] leading-5 text-fg-mute">
                    git 仓库从团队信息自动匹配,需先在本机检出团队仓库。
                  </p>
                </div>
              </div>
              <div className="min-h-0 overflow-y-auto ace-scrollbar px-5 py-4 space-y-4">
                <label className="block">
                  <span className="block mb-1.5 text-[12px] font-medium text-fg">邀请链接</span>
                  <input
                    value={joinModal.link}
                    onChange={(e) => setJoinModal((prev) => ({ ...prev, link: e.target.value, error: '' }))}
                    autoComplete="off"
                    spellCheck={false}
                    placeholder="acecode://team/…"
                    className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] font-mono text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
                  />
                </label>
                <label className="block">
                  <span className="block mb-1.5 text-[12px] font-medium text-fg">工作目录</span>
                  <input
                    value={joinModal.cwd}
                    onChange={(e) => setJoinModal((prev) => ({ ...prev, cwd: e.target.value, error: '' }))}
                    autoComplete="off"
                    spellCheck={false}
                    placeholder="/home/user/project"
                    className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] font-mono text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
                  />
                </label>
                {joinModal.error && (
                  <div role="alert" className="px-3 py-2 rounded-lg border border-danger/30 bg-surface text-[12px] leading-5 text-danger">
                    {joinModal.error}
                  </div>
                )}
              </div>
              <div className="shrink-0 px-5 py-3 border-t border-border bg-surface-alt flex items-center justify-end gap-2">
                <button
                  type="button"
                  onClick={close}
                  className="h-8 px-3 rounded-lg text-[12px] text-fg hover:bg-surface-hi"
                >取消</button>
                <button
                  type="submit"
                  data-ace-dialog-primary="true"
                  disabled={joinModal.busy}
                  className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 flex items-center justify-center gap-1.5"
                >
                  {joinModal.busy && <span className="ace-spinner w-3 h-3" />}
                  {joinModal.busy ? '加入中…' : '加入团队'}
                </button>
              </div>
            </form>
          )}
        </Modal>
      )}
    </div>
  );
}
