// 团队设置(设置 > 集成 > 团队,add-team-workdir-sidebar 反馈轮两级导航)。
// 第一级(root):功能开关(默认关,即时生效)、本机账号每日 token 额度、
//   团队列表(点行进入第二级)与创建/加入入口。
// 第二级(detail):单个团队——基本信息(工作区位置/仓库)、邀请码、成员
//   与角色、团队会话列表(发起/接管/消息)。状态经 /ws/team/<id> 实时合并,
//   纯逻辑在 lib/teamSettings.js。
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
  teamErrorText as errText,
  toggleStreamExpanded,
  visibleStreamLines,
} from '../lib/teamSettings.js';
import { pickWorkspaceFolder } from '../lib/workspaceFolderPicker.js';
import { Modal, Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const NOW_MS_INTERVAL = 1000;
const QUOTA_MILLION = 1_000_000;

// 设置页二级/一级共用:状态圆点。

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

function TeamSettingsDetail({ teamId, onBack }) {
  const [state, setState] = useState(initialTeamSettingsState);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const busyRef = useRef('');
  const [invite, setInvite] = useState(null); // { link, expires_at, team_id }
  const [nowSeconds, setNowSeconds] = useState(() => Math.floor(Date.now() / 1000));
  const [takeoverTarget, setTakeoverTarget] = useState(null); // { teamId, sessionId }
  const [notice, setNotice] = useState(null); // { text, tone }
  const [leaveOpen, setLeaveOpen] = useState(false);
  const [leaving, setLeaving] = useState(false);
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

  // 二级视图锁定在该团队:进入时定位,加载失败提示返回。
  useEffect(() => {
    setState((prev) => ({ ...prev, activeTeamId: teamId }));
    load();
  }, [load, teamId]);

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

  const issueInvite = () => {
    if (!activeTeam) return;
    run('invite', async () => {
      const result = await api.issueTeamInvite(activeTeam.team_id);
      setInvite({ ...result, team_id: activeTeam.team_id, issued_at: nowSeconds });
      toast({ kind: 'ok', text: '邀请码已生成,15 分钟内有效且只能使用一次' });
      return result;
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

  // 工作区位置:目录选择器(原生/网页)→ 保存本机团队工作目录;与团队已
  // 登记仓库匹配时 daemon 侧自动补登记。
  const changeWorkdir = async () => {
    if (busy === 'workdir' || !activeTeam) return;
    setBusy('workdir');
    try {
      const picked = await pickWorkspaceFolder({ api });
      if (!picked) return;
      await api.setTeamWorkdir(activeTeam.team_id, picked);
      await load();
      toast({ kind: 'ok', text: '工作区位置已更新' });
    } catch (e) {
      toast({ kind: 'err', text: errText(e) });
    } finally {
      setBusy('');
    }
  };

  const confirmLeave = async () => {
    if (!activeTeam || leaving) return;
    setLeaving(true);
    try {
      await api.leaveTeam(activeTeam.team_id);
      toast({ kind: 'ok', text: '已退出团队，团队信息与会话已归档' });
      window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
      setLeaveOpen(false);
      onBack();
    } catch (e) {
      toast({ kind: 'err', text: errText(e) });
    } finally {
      setLeaving(false);
    }
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
        <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 text-[13px] text-fg-mute max-w-md">
          未找到该团队,可能已被移除。
        </div>
      )}

      {activeTeam && (
        <>
          <div className="flex items-center justify-between mb-3">
            <div className="flex items-center gap-2 text-[14px] text-fg min-w-0">
              <button
                type="button"
                onClick={onBack}
                className="w-7 h-7 rounded-md flex items-center justify-center text-fg-mute hover:text-fg hover:bg-surface-hi shrink-0"
                title="返回团队列表"
                aria-label="返回团队列表"
              >
                <VsIcon name="back" size={18} />
              </button>
              <span className="min-w-0 truncate">{activeTeam.name || '团队'}</span>
            </div>
            <Dot
              tone={state.wsStatus === 'open' ? 'ok' : 'warn'}
              label={state.wsStatus === 'open' ? '已连接' : '连接中…'}
            />
          </div>

          {/* 基本信息:工作区位置(可改) + 团队仓库 */}
          <div className="text-[14px] font-semibold mt-1 mb-2">基本信息</div>
          <div className="flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
            <div className="text-[13px] text-fg shrink-0">工作区位置</div>
            <div className="flex items-center gap-2 min-w-0">
              <div
                className="text-[12px] text-fg-mute truncate max-w-[20rem]"
                title={activeTeam.local?.work_dir || ''}
              >
                {activeTeam.local?.work_dir || '未设置'}
              </div>
              <button
                type="button"
                disabled={busy === 'workdir'}
                onClick={changeWorkdir}
                className="px-2 py-0.5 text-[11px] text-fg-mute hover:text-fg hover:underline shrink-0 disabled:opacity-50"
              >选择目录</button>
            </div>
          </div>
          {(activeTeam.repos || []).length > 0 && (
            <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
              <div className="text-[13px] text-fg shrink-0">团队仓库</div>
              <div
                className="text-[12px] text-fg-mute truncate max-w-[24rem]"
                title={(activeTeam.repos || []).map((r) => r.value).join(' · ')}
              >
                {(activeTeam.repos || []).map((r) => r.value).join(' · ')}
              </div>
            </div>
          )}

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
                    <span className="text-[11px] text-fg-mute">额度在团队设置第一级配置</span>
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

          <div className="mt-6 pt-4 border-t border-border max-w-xl">
            <button
              type="button"
              onClick={() => setLeaveOpen(true)}
              className="text-[12px] text-danger hover:underline"
            >退出团队</button>
          </div>
        </>
      )}

      {leaveOpen && (
        <Modal
          onClose={() => !leaving && setLeaveOpen(false)}
          dismissOnBackdrop={!leaving}
          dismissOnEscape={!leaving}
          labelledBy="team-leave-title"
        >
          <div className="p-5 max-w-[26rem]">
            <h3 id="team-leave-title" className="text-[15px] font-semibold text-fg">退出团队</h3>
            <p className="mt-2 text-[12px] leading-5 text-fg-2">
              退出后本机团队会话和团队信息会进入归档，其他队员仍保留该团队。归档后可以直接删除本机这份团队数据。
            </p>
            <div className="mt-5 flex justify-end gap-2">
              <button
                type="button"
                disabled={leaving}
                onClick={() => setLeaveOpen(false)}
                className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi disabled:opacity-50"
              >取消</button>
              <button
                type="button"
                disabled={leaving}
                onClick={confirmLeave}
                className="h-8 rounded-md bg-danger px-3 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-50"
              >{leaving ? '退出中…' : '退出团队'}</button>
            </div>
          </div>
        </Modal>
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

// ── 第一级:团队功能整体设置 ─────────────────────────────────────────
export function TeamSettings() {
  const [view, setView] = useState('root'); // root = 整体设置;detail = 单个团队
  const [detailTeamId, setDetailTeamId] = useState('');

  // 侧边栏「团队设置」入口:直接进入该团队的二级设置。
  useEffect(() => {
    const handler = (event) => {
      const teamId = event.detail?.teamId;
      if (!teamId) return;
      setDetailTeamId(teamId);
      setView('detail');
    };
    window.addEventListener('acecode:open-team-settings', handler);
    return () => window.removeEventListener('acecode:open-team-settings', handler);
  }, []);

  if (view === 'detail') {
    return (
      <TeamSettingsDetail
        teamId={detailTeamId}
        onBack={() => setView('root')}
      />
    );
  }
  return <TeamSettingsRoot onOpenTeam={(teamId) => { setDetailTeamId(teamId); setView('detail'); }} />;
}

function TeamSettingsRoot({ onOpenTeam }) {
  const [config, setConfig] = useState(null); // { enabled, account_daily_token_quota, account_quota_used_today }
  const [configLoading, setConfigLoading] = useState(true);
  const [teams, setTeams] = useState([]);
  const [archivedTeams, setArchivedTeams] = useState([]);
  const [teamsLoading, setTeamsLoading] = useState(true);
  const [saving, setSaving] = useState(''); // 'enabled' | 'quota'
  const [quotaDraft, setQuotaDraft] = useState(null); // 百万 tokens 字符串草稿
  const [createOpen, setCreateOpen] = useState(false);
  const [joinOpen, setJoinOpen] = useState(false);
  const [deleteTarget, setDeleteTarget] = useState(null); // 归档团队
  const [deleting, setDeleting] = useState(false);
  const enabled = config?.enabled === true;

  const load = useCallback(async () => {
    try {
      const result = await api.getTeamConfig();
      setConfig({
        enabled: result?.enabled === true,
        account_daily_token_quota: Number(result?.account_daily_token_quota ?? 0),
        account_quota_used_today: Number(result?.account_quota_used_today ?? 0),
      });
    } catch (e) {
      toast({ kind: 'err', text: errText(e) });
    } finally {
      setConfigLoading(false);
    }
    try {
      const result = await api.listTeams();
      setTeams(Array.isArray(result?.teams) ? result.teams : []);
    } catch { /* 功能未开或暂不可用:列表为空 */ }
    try {
      const result = await api.listArchivedTeams();
      setArchivedTeams(Array.isArray(result?.teams) ? result.teams : []);
    } catch { setArchivedTeams([]); }
    finally {
      setTeamsLoading(false);
    }
  }, []);

  useEffect(() => { load(); }, [load]);
  useEffect(() => {
    window.addEventListener('acecode:teams-changed', load);
    window.addEventListener('acecode:team-config-changed', load);
    return () => {
      window.removeEventListener('acecode:teams-changed', load);
      window.removeEventListener('acecode:team-config-changed', load);
    };
  }, [load]);

  const notifyTeamConfigChanged = () => {
    window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
    window.dispatchEvent(new CustomEvent('acecode:team-config-changed'));
  };

  // 开关:变更立即持久化并即时启停 daemon 团队模块;失败回滚。
  const switchEnabled = async (next) => {
    if (saving) return;
    const previous = config;
    setConfig((prev) => ({ ...prev, enabled: next }));
    setSaving('enabled');
    try {
      const result = await api.setTeamConfig({ enabled: next });
      setConfig((prev) => ({
        ...prev,
        enabled: result?.enabled === true,
        account_daily_token_quota: Number(result?.account_daily_token_quota ?? prev?.account_daily_token_quota ?? 0),
      }));
      notifyTeamConfigChanged();
    } catch (e) {
      setConfig(previous);
      toast({ kind: 'err', text: errText(e) });
    } finally {
      setSaving('');
    }
  };

  // 账号每日额度:失焦 / Enter 提交(百万 tokens);失败保留草稿可重试。
  const saveQuota = async () => {
    if (saving || quotaDraft == null) return;
    const value = Math.floor(Number(quotaDraft) * QUOTA_MILLION);
    if (!Number.isFinite(value) || value < 0) {
      toast({ kind: 'err', text: '额度无效:请输入不小于 0 的百万 tokens 数,0 表示当日不承接' });
      return;
    }
    setSaving('quota');
    try {
      const result = await api.setTeamConfig({ account_daily_token_quota: value });
      setConfig((prev) => ({
        ...prev,
        account_daily_token_quota: Number(result?.account_daily_token_quota ?? value),
      }));
      setQuotaDraft(null);
      notifyTeamConfigChanged();
    } catch (e) {
      toast({ kind: 'err', text: errText(e) }); // 草稿保留,可直接重试
    } finally {
      setSaving('');
    }
  };

  const afterCreated = async (meta) => {
    notifyTeamConfigChanged();
    onOpenTeam(meta.team_id);
  };

  const confirmDeleteArchived = async () => {
    if (!deleteTarget || deleting) return;
    setDeleting(true);
    try {
      await api.deleteTeam(deleteTarget.team_id);
      setArchivedTeams((prev) => prev.filter((team) => team.team_id !== deleteTarget.team_id));
      setDeleteTarget(null);
      window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
      toast({ kind: 'ok', text: `已删除“${deleteTarget.name || deleteTarget.team_id}”` });
    } catch (e) {
      toast({ kind: 'err', text: errText(e) });
    } finally {
      setDeleting(false);
    }
  };

  return (
    <div className="flex-1 overflow-y-auto px-12 py-6">
      <h2 className="text-xl font-bold mb-5">团队</h2>

      {configLoading ? (
        <div className="text-[13px] text-fg-mute">加载中…</div>
      ) : (
        <div className="max-w-xl">
          {/* 1. 功能开关:关闭时 daemon 停用团队模块,主界面不出现团队栏目 */}
          <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
            <div className="text-[13px] text-fg">开启团队功能</div>
            <Toggle
              on={enabled}
              disabled={saving === 'enabled'}
              onChange={(value) => switchEnabled(value)}
              ariaLabel="开启团队功能"
            />
          </div>

          {enabled && (
            <>
              {/* 2. 本机账号的每日 token 额度:账号下所有团队共享同一个池 */}
              <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
                <div>
                  <div className="text-[13px] text-fg">每日 token 额度</div>
                  <div className="text-[11px] text-fg-mute mt-0.5">
                    本机账号所有团队共享,按本机时间重置 · 已用 {quotaUsedText(config.account_quota_used_today)}
                  </div>
                </div>
                <div className="flex items-center gap-1.5">
                  <input
                    type="number"
                    min="0"
                    value={quotaDraft ?? (config.account_daily_token_quota ?? 0) / QUOTA_MILLION}
                    onChange={(e) => setQuotaDraft(e.target.value)}
                    onBlur={saveQuota}
                    onKeyDown={(e) => { if (e.key === 'Enter') saveQuota(); }}
                    aria-label="每日 token 额度(百万 tokens)"
                    disabled={saving === 'quota'}
                    className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
                  />
                  <span className="text-[12px] text-fg-mute">M</span>
                </div>
              </div>

              {/* 3. 团队列表:点行进入该团队的二级设置 */}
              <div className="text-[14px] font-semibold mt-4 mb-2">团队列表</div>
              {teamsLoading && <div className="text-[13px] text-fg-mute mb-2">加载中…</div>}
              {!teamsLoading && teams.length === 0 && archivedTeams.length === 0 && (
                <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 text-[13px] text-fg-mute">
                  还没有团队。
                </div>
              )}
              {teams.map((team) => (
                <div
                  key={team.team_id}
                  role="button"
                  tabIndex={0}
                  onClick={() => onOpenTeam(team.team_id)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      e.preventDefault();
                      onOpenTeam(team.team_id);
                    }
                  }}
                  className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 cursor-pointer hover:bg-surface-hi transition"
                >
                  <div className="min-w-0">
                    <div className="text-[13px] text-fg truncate">{team.name || team.team_id}</div>
                    <div className="text-[11px] text-fg-mute mt-0.5">
                      {(team.members || []).length} 名成员 · {(team.sessions || []).length} 个团队会话
                    </div>
                  </div>
                  <VsIcon name="expandRight" size={16} className="text-fg-mute shrink-0" />
                </div>
              ))}

              {archivedTeams.length > 0 && (
                <>
                  <div className="text-[14px] font-semibold mt-4 mb-2">已归档团队</div>
                  {archivedTeams.map((team) => {
                    const archivedAt = team.local?.archived_at
                      ? new Date(team.local.archived_at * 1000).toLocaleDateString()
                      : '';
                    return (
                      <div
                        key={team.team_id}
                        className="flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2"
                      >
                        <div className="min-w-0">
                          <div className="text-[13px] text-fg truncate">{team.name || team.team_id}</div>
                          <div className="text-[11px] text-fg-mute mt-0.5">
                            {(team.members || []).length} 名成员 · {(team.sessions || []).length} 个团队会话
                            {archivedAt ? ` · 归档于 ${archivedAt}` : ''}
                          </div>
                        </div>
                        <button
                          type="button"
                          onClick={() => setDeleteTarget(team)}
                          className="px-2 py-0.5 text-[11px] text-danger hover:underline shrink-0"
                        >删除</button>
                      </div>
                    );
                  })}
                </>
              )}

              <div className="flex items-center gap-2 mt-3">
                <button
                  type="button"
                  onClick={() => setCreateOpen(true)}
                  className="px-3 py-1 bg-accent text-white rounded"
                >创建团队</button>
                <button
                  type="button"
                  onClick={() => setJoinOpen(true)}
                  className="px-3 py-1 rounded border border-border text-[13px] text-fg hover:bg-surface-hi transition"
                >加入团队</button>
              </div>
            </>
          )}
        </div>
      )}

      {createOpen && (
        <TeamCreateModal onClose={() => setCreateOpen(false)} onCreated={afterCreated} />
      )}
      {joinOpen && (
        <TeamJoinModal onClose={() => setJoinOpen(false)} onJoined={notifyTeamConfigChanged} />
      )}
      {deleteTarget && (
        <Modal
          onClose={() => !deleting && setDeleteTarget(null)}
          dismissOnBackdrop={!deleting}
          dismissOnEscape={!deleting}
          labelledBy="team-archive-delete-title"
        >
          <div className="p-5 max-w-[26rem]">
            <h3 id="team-archive-delete-title" className="text-[15px] font-semibold text-fg">
              删除已归档团队
            </h3>
            <p className="mt-2 text-[12px] leading-5 text-fg-2">
              将永久删除“{deleteTarget.name || deleteTarget.team_id}”的团队信息和本机归档的团队会话，其他设备不受影响。
            </p>
            <div className="mt-5 flex justify-end gap-2">
              <button
                type="button"
                disabled={deleting}
                onClick={() => setDeleteTarget(null)}
                className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi disabled:opacity-50"
              >取消</button>
              <button
                type="button"
                disabled={deleting}
                onClick={confirmDeleteArchived}
                className="h-8 rounded-md bg-danger px-3 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-50"
              >{deleting ? '删除中…' : '删除'}</button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}

