// 创建/加入团队弹框(add-team-workdir-sidebar)。
// 设置页与侧边栏「团队」分组的悬停入口共用;创建=名称+工作目录+可选仓库
// URL(工作目录是 git 仓库时从目录解析,否则必填),加入=邀请链接+工作目录
// (git 仓库从团队信息自动匹配)。工作目录支持原生/网页目录选择
// (workspaceFolderPicker 同一链路),不要求用户手输完整路径。
import { useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { teamErrorText } from '../lib/teamSettings.js';
import { pickWorkspaceFolder } from '../lib/workspaceFolderPicker.js';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const inputClassName =
  'w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60';

// 工作目录字段:输入 + 目录选择按钮(Desktop 原生对话框 / 网页选择器)。
function WorkDirField({ value, onChange, disabled }) {
  const [picking, setPicking] = useState(false);
  const pick = async () => {
    if (picking) return;
    setPicking(true);
    try {
      const picked = await pickWorkspaceFolder({ api });
      if (picked) onChange(picked);
    } catch (error) {
      toast({ kind: 'err', text: error?.message || '选择目录失败' });
    } finally {
      setPicking(false);
    }
  };
  return (
    <div className="flex items-center gap-2">
      <input
        value={value}
        onChange={(e) => onChange(e.target.value)}
        autoComplete="off"
        spellCheck={false}
        placeholder="/home/user/project"
        disabled={disabled}
        className={`${inputClassName} flex-1 min-w-0`}
      />
      <button
        type="button"
        onClick={pick}
        disabled={disabled || picking}
        className="h-9 px-3 rounded-lg border border-border text-[12px] text-fg hover:bg-surface-hi transition disabled:opacity-50 shrink-0"
      >
        {picking ? '选择中…' : '选择目录'}
      </button>
    </div>
  );
}

export function TeamCreateModal({ onClose, onCreated }) {
  const nameRef = useRef(null);
  const [name, setName] = useState('');
  const [cwd, setCwd] = useState('');
  const [repoUrl, setRepoUrl] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const submit = async (close) => {
    if (busy) return;
    if (!name.trim()) { setError('请填写团队名称'); return; }
    if (!cwd.trim()) { setError('请填写工作目录'); return; }
    setBusy(true);
    setError('');
    try {
      const meta = await api.createTeam(name.trim(), { cwd: cwd.trim(), repoUrl: repoUrl.trim() });
      window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
      toast({ kind: 'ok', text: '团队已创建' });
      await onCreated?.(meta);
      close();
    } catch (e) {
      setError(teamErrorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal onClose={onClose} width={520} dismissOnBackdrop={false} labelledBy="team-create-title">
      {({ close }) => (
        <form onSubmit={(event) => { event.preventDefault(); submit(close); }} className="ace-modal-form">
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
                ref={nameRef}
                value={name}
                onChange={(e) => { setName(e.target.value); setError(''); }}
                maxLength={128}
                autoComplete="off"
                spellCheck={false}
                className={inputClassName}
              />
            </label>
            <label className="block">
              <span className="block mb-1.5 text-[12px] font-medium text-fg">工作目录</span>
              <WorkDirField value={cwd} onChange={(v) => { setCwd(v); setError(''); }} disabled={busy} />
            </label>
            <label className="block">
              <span className="block mb-1.5 text-[12px] font-medium text-fg">git 仓库 URL(选填)</span>
              <input
                value={repoUrl}
                onChange={(e) => { setRepoUrl(e.target.value); setError(''); }}
                autoComplete="off"
                spellCheck={false}
                placeholder="https://github.com/org/repo.git"
                disabled={busy}
                className={inputClassName}
              />
              <span className="block mt-1.5 text-[11px] leading-5 text-fg-mute">
                工作目录是 git 仓库时自动从目录解析;不是 git 仓库时必填。
              </span>
            </label>
            {error && (
              <div role="alert" className="px-3 py-2 rounded-lg border border-danger/30 bg-surface text-[12px] leading-5 text-danger">
                {error}
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
              disabled={busy}
              className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 flex items-center justify-center gap-1.5"
            >
              {busy && <span className="ace-spinner w-3 h-3" />}
              {busy ? '创建中…' : '创建团队'}
            </button>
          </div>
        </form>
      )}
    </Modal>
  );
}

export function TeamJoinModal({ onClose, onJoined }) {
  const [link, setLink] = useState('');
  const [cwd, setCwd] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const submit = async (close) => {
    if (busy) return;
    if (!link.trim()) { setError('请粘贴邀请链接'); return; }
    if (!cwd.trim()) { setError('请填写工作目录'); return; }
    setBusy(true);
    setError('');
    try {
      await api.joinTeam(link.trim(), cwd.trim());
      window.dispatchEvent(new CustomEvent('acecode:teams-changed'));
      toast({ kind: 'ok', text: '已加入团队' });
      await onJoined?.();
      close();
    } catch (e) {
      setError(teamErrorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal onClose={onClose} width={520} dismissOnBackdrop={false} labelledBy="team-join-title">
      {({ close }) => (
        <form onSubmit={(event) => { event.preventDefault(); submit(close); }} className="ace-modal-form">
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
                value={link}
                onChange={(e) => { setLink(e.target.value); setError(''); }}
                autoComplete="off"
                spellCheck={false}
                placeholder="acecode://team/…"
                disabled={busy}
                className={inputClassName}
              />
            </label>
            <label className="block">
              <span className="block mb-1.5 text-[12px] font-medium text-fg">工作目录</span>
              <WorkDirField value={cwd} onChange={(v) => { setCwd(v); setError(''); }} disabled={busy} />
            </label>
            {error && (
              <div role="alert" className="px-3 py-2 rounded-lg border border-danger/30 bg-surface text-[12px] leading-5 text-danger">
                {error}
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
              disabled={busy}
              className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 flex items-center justify-center gap-1.5"
            >
              {busy && <span className="ace-spinner w-3 h-3" />}
              {busy ? '加入中…' : '加入团队'}
            </button>
          </div>
        </form>
      )}
    </Modal>
  );
}
