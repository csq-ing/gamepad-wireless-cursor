import type { CSSProperties } from "react";
import { Check, RotateCcw, SlidersHorizontal, X } from "lucide-react";

type Language = "zh" | "en";

type StickConfigCopy = {
  title: string;
  subtitle: string;
  stick: string;
  deadzone: string;
  rawValue: string;
  reset: string;
  apply: string;
  saving: string;
  close: string;
  low: string;
  high: string;
};

const COPY: Record<Language, StickConfigCopy> = {
  zh: {
    title: "摇杆配置",
    subtitle: "调整摇杆死区，过滤轻微漂移并保留细腻输入。",
    stick: "摇杆",
    deadzone: "死区",
    rawValue: "原始值",
    reset: "重置",
    apply: "应用",
    saving: "应用中",
    close: "关闭",
    low: "灵敏",
    high: "稳定"
  },
  en: {
    title: "Stick Config",
    subtitle: "Tune the stick deadzone to filter drift while keeping fine control.",
    stick: "Stick",
    deadzone: "Deadzone",
    rawValue: "Raw",
    reset: "Reset",
    apply: "Apply",
    saving: "Applying",
    close: "Close",
    low: "Sensitive",
    high: "Stable"
  }
};

type StickConfigPanelProps = {
  language: Language;
  deadzone: number;
  maxDeadzone?: number;
  isSaving?: boolean;
  statusMessage?: string | null;
  onDeadzoneChange: (value: number) => void;
  onReset: () => void;
  onApply: () => void;
  onClose: () => void;
};

function clampDeadzone(value: number, maxDeadzone: number) {
  if (!Number.isFinite(value)) {
    return 0;
  }

  return Math.min(maxDeadzone, Math.max(0, Math.round(value)));
}

function DeadzoneControl({
  label,
  value,
  maxDeadzone,
  copy,
  onChange
}: {
  label: string;
  value: number;
  maxDeadzone: number;
  copy: StickConfigCopy;
  onChange: (value: number) => void;
}) {
  const normalizedValue = clampDeadzone(value, maxDeadzone);
  const percent = Math.round((normalizedValue / Math.max(1, maxDeadzone)) * 100);
  const rangeStyle = { "--stick-progress": `${percent}%` } as CSSProperties;

  return (
    <section className="px-5 py-5">
      <div className="mb-4 flex items-center justify-between gap-4">
        <div>
          <h3 className="text-[15px] font-semibold text-slate-900">{label}</h3>
          <p className="mt-1 text-xs font-medium text-slate-500">{copy.deadzone}</p>
        </div>

        <div className="flex items-center gap-2 rounded-lg border border-slate-200 bg-slate-50 px-2 py-1.5">
          <span className="text-xs font-semibold text-slate-500">{copy.rawValue}</span>
          <input
            type="number"
            min={0}
            max={maxDeadzone}
            value={normalizedValue}
            onChange={(event) => onChange(clampDeadzone(Number(event.target.value), maxDeadzone))}
            className="h-7 w-16 rounded-md border border-slate-200 bg-white px-2 text-right text-xs font-semibold text-slate-800 outline-none transition focus:border-[#7a5bff]"
          />
        </div>
      </div>

      <div className="flex items-center gap-4">
        <span className="w-14 text-right text-[11px] font-semibold uppercase text-slate-400">{copy.low}</span>
        <div className="flex flex-1 items-center">
          <input
            type="range"
            min={0}
            max={maxDeadzone}
            step={8}
            value={normalizedValue}
            onChange={(event) => onChange(Number(event.target.value))}
            style={rangeStyle}
            className="stick-deadzone-range w-full cursor-pointer"
          />
        </div>
        <span className="w-12 text-[11px] font-semibold uppercase text-slate-400">{copy.high}</span>
      </div>
    </section>
  );
}

export function StickConfigPanel({
  language,
  deadzone,
  maxDeadzone = 1024,
  isSaving = false,
  statusMessage,
  onDeadzoneChange,
  onReset,
  onApply,
  onClose
}: StickConfigPanelProps) {
  const copy = COPY[language];

  return (
    <aside className="pointer-events-auto absolute right-10 top-28 z-20 flex w-[440px] flex-col overflow-hidden rounded-2xl border border-slate-200 bg-white shadow-[0_24px_70px_rgba(15,23,42,0.16)]">
      <div className="flex items-start justify-between gap-5 border-b border-slate-100 px-5 py-5">
        <div className="flex min-w-0 gap-3">
          <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-xl bg-[#7a5bff] text-white">
            <SlidersHorizontal className="h-5 w-5" strokeWidth={2.2} />
          </div>
          <div>
            <h2 className="text-lg font-semibold text-slate-950">{copy.title}</h2>
            <p className="mt-1 max-w-[320px] text-sm leading-5 text-slate-500">{copy.subtitle}</p>
          </div>
        </div>

        <button
          type="button"
          onClick={onClose}
          className="flex h-8 w-8 shrink-0 items-center justify-center rounded-lg text-slate-400 transition hover:bg-slate-100 hover:text-slate-700"
          aria-label={copy.close}
        >
          <X className="h-4 w-4" />
        </button>
      </div>

      <DeadzoneControl
        label={copy.stick}
        value={deadzone}
        maxDeadzone={maxDeadzone}
        copy={copy}
        onChange={onDeadzoneChange}
      />

      <div className="border-t border-slate-100 px-5 py-4">
        {statusMessage ? <p className="mb-3 text-sm font-medium text-slate-500">{statusMessage}</p> : null}
        <div className="flex items-center justify-between gap-3">
          <button
            type="button"
            onClick={onReset}
            className="inline-flex items-center gap-2 rounded-lg border border-slate-200 bg-white px-4 py-2.5 text-sm font-semibold text-slate-700 transition hover:border-slate-300 hover:bg-slate-50"
          >
            <RotateCcw className="h-4 w-4" />
            {copy.reset}
          </button>

          <button
            type="button"
            onClick={onApply}
            disabled={isSaving}
            className="inline-flex min-w-28 items-center justify-center gap-2 rounded-lg bg-slate-900 px-4 py-2.5 text-sm font-semibold text-white transition hover:bg-black disabled:cursor-wait disabled:bg-slate-500"
          >
            <Check className="h-4 w-4" />
            {isSaving ? copy.saving : copy.apply}
          </button>
        </div>
      </div>
    </aside>
  );
}
