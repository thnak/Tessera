"""Resolve scene_session.cpp: keep main's measurer AND #408's posture path.

#428 changed the layout call to take a measurer; #408 added `set_posture` and changed the metrics
plumbing. Both are needed, so the union has to compose them rather than pick a side: main's
`layout_scene(..., measurer)` signature with #408's posture field and no-error early return.
"""

import pathlib

p = pathlib.Path('tools/codegen/scene_session.cpp')
t = p.read_text(encoding='utf-8')

start = t.index('<<<<<<< HEAD')
mid = t.index('=======', start)
end = t.index('>>>>>>>', mid)
nl = t.index('\n', end) + 1

ours = t[t.index('\n', start) + 1:mid]
theirs = t[mid + len('=======\n'):end]

# main's side is `theirs`: the measurer-aware call. #408's side is `ours`: metrics + set_posture.
# Compose: use the measurer-aware signature, keep the posture member and set_posture.
composed = (
    "    ui::layout::LayoutEngine engine(options_.measurer);\n"
    "    const SceneLayoutResult laid_out =\n"
    "        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer);\n"
    "    metrics_ = laid_out.metrics;\n"
    "}\n"
    "\n"
    "bool PreviewSession::set_posture(hal::FoldPosture posture) {\n"
    "    if (!built_ || layout_metrics_.posture == posture) {\n"
    "        return false;\n"
    "    }\n"
    "    layout_metrics_.posture = posture;\n"
    "    ui::layout::LayoutEngine engine(options_.measurer);\n"
    "    const SceneLayoutResult laid_out =\n"
    "        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer);\n"
    "    if (!laid_out.error.empty()) {\n"
    "        // The tree stays at the previous posture rather than half-applied: the error is a layout\n"
    "        // failure, not a posture the caller asked for, and the metrics are left describing what\n"
    "        // the boxes actually are.\n"
    "        return false;\n"
    "    }\n"
    "    metrics_ = laid_out.metrics;\n"
    "    return true;\n"
)
t = t[:start] + composed + t[nl:]
p.write_text(t, encoding='utf-8', newline='')
print('composed; markers left:', t.count('<<<<<<<'))
print('layout_scene signature must be checked next')
