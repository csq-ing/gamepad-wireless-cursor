type Language = "zh" | "en";

type MotionMappingCopy = {
  viewTitle: string;
  viewDeadzone: string;
  viewSensitivity: string;
  castTitle: string;
  castDeadzone: string;
};

const COPY: Record<Language, MotionMappingCopy> = {
  zh: {
    viewTitle: "视角控制",
    viewDeadzone: "死区",
    viewSensitivity: "灵敏度",
    castTitle: "抛竿",
    castDeadzone: "死区"
  },
  en: {
    viewTitle: "View Control",
    viewDeadzone: "Deadzone",
    viewSensitivity: "Sensitivity",
    castTitle: "Casting",
    castDeadzone: "Deadzone"
  }
};

type MotionMappingPanelProps = {
  language: Language;
  viewDeadzone: number;
  viewSensitivity: number;
  castDeadzone: number;
  onViewDeadzoneChange: (value: number) => void;
  onViewSensitivityChange: (value: number) => void;
  onCastDeadzoneChange: (value: number) => void;
};

type MotionRangeProps = {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  displayValue: string;
  minLabel: string;
  maxLabel: string;
  onChange: (value: number) => void;
};

function clamp(value: number, min: number, max: number) {
  if (!Number.isFinite(value)) {
    return min;
  }

  return Math.min(max, Math.max(min, value));
}

function MotionRange({
  label,
  value,
  min,
  max,
  step,
  displayValue,
  minLabel,
  maxLabel,
  onChange
}: MotionRangeProps) {
  const normalizedValue = clamp(value, min, max);

  return (
    <div className="border-t border-slate-100 px-5 py-5 first:border-t-0">
      <div className="mb-4 flex items-center justify-between gap-5">
        <p className="text-[15px] font-medium text-slate-900">{label}</p>
        <div className="rounded-lg border border-slate-200 bg-slate-50 px-3 py-1.5 font-mono text-sm font-medium tracking-[0.02em] text-slate-800">
          {displayValue}
        </div>
      </div>

      <div className="flex items-center gap-4">
        <span className="w-10 text-right text-[11px] font-semibold uppercase text-slate-400">{minLabel}</span>
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={normalizedValue}
          onChange={(event) => onChange(Number(event.target.value))}
          className="h-2 w-full cursor-pointer accent-[#7a5bff]"
        />
        <span className="w-10 text-[11px] font-semibold uppercase text-slate-400">{maxLabel}</span>
      </div>
    </div>
  );
}

export function MotionMappingPanel({
  language,
  viewDeadzone,
  viewSensitivity,
  castDeadzone,
  onViewDeadzoneChange,
  onViewSensitivityChange,
  onCastDeadzoneChange
}: MotionMappingPanelProps) {
  const copy = COPY[language];

  return (
    <div className="flex w-full justify-start px-3 pb-16 pt-36 sm:px-4">
      <div className="w-full max-w-[980px]">
        <div className="grid gap-12">
          <section>
            <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{copy.viewTitle}</h2>

            <div className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
              <MotionRange
                label={copy.viewDeadzone}
                value={viewDeadzone}
                min={0}
                max={8}
                step={0.1}
                displayValue={`${viewDeadzone.toFixed(1)}°`}
                minLabel="0°"
                maxLabel="8°"
                onChange={onViewDeadzoneChange}
              />

              <MotionRange
                label={copy.viewSensitivity}
                value={viewSensitivity}
                min={0.2}
                max={2.5}
                step={0.1}
                displayValue={`${viewSensitivity.toFixed(1)}x`}
                minLabel="0.2x"
                maxLabel="2.5x"
                onChange={onViewSensitivityChange}
              />
            </div>
          </section>

          <section>
            <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{copy.castTitle}</h2>

            <div className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
              <MotionRange
                label={copy.castDeadzone}
                value={castDeadzone}
                min={0}
                max={8}
                step={0.1}
                displayValue={`${castDeadzone.toFixed(1)}°`}
                minLabel="0°"
                maxLabel="8°"
                onChange={onCastDeadzoneChange}
              />
            </div>
          </section>
        </div>
      </div>
    </div>
  );
}
