// SPDX-License-Identifier: MIT
//
// Folder move and folder delete plans (issue #391), tested without a browser:
//
//   node scripts/ci/preview_project_folder_ops.mjs
import assert from 'node:assert/strict';

import { filesUnder, planFolderDelete, planFolderMove } from '../../designer/web/project-folder-ops.js';

const files = [
  { path: 'tessera.json', editable: true },
  { path: 'ui/App.tsx', editable: true },
  { path: 'ui/parts/Row.tsx', editable: true },
  { path: 'styles/app.css', editable: true },
  { path: 'gen/AppBase.hpp', editable: false },
  { path: 'assets/logo.png', editable: false },
];
let checks = 0;
const check = (name, ok, detail = '') => {
  assert.ok(ok, name + ' ' + detail);
  checks += 1;
};

check('filesUnder is prefix with a slash', filesUnder(files, 'ui').length === 2 && filesUnder(files, 'u').length === 0);

const move = planFolderMove(files, 'ui', 'app/screens');
check('a folder move maps every file, nested ones included', move.ok && move.moves.length === 2 &&
  move.moves.some((m) => m.from === 'ui/parts/Row.tsx' && m.to === 'app/screens/parts/Row.tsx') &&
  move.moves.some((m) => m.from === 'ui/App.tsx' && m.to === 'app/screens/App.tsx'));
check('a rename keeps the parent', planFolderMove(files, 'ui/parts', 'ui/cells').moves[0].to === 'ui/cells/Row.tsx');

const refused = (plan, pattern, name) => check(name, plan.ok === false && pattern.test(plan.reason), JSON.stringify(plan));
refused(planFolderMove(files, 'ui', '../out'), /\.\./, 'a move that escapes the project is refused');
refused(planFolderMove(files, 'ui', '/abs'), /./, 'an absolute target is refused');
refused(planFolderMove(files, 'ui', 'a\\b'), /backslash/, 'a backslash target is refused');
refused(planFolderMove(files, 'ui', 'ui/inner'), /into itself/, 'a folder cannot move into itself');
refused(planFolderMove(files, 'ui', 'ui'), /already there/, 'a same-path move is refused');
refused(planFolderMove(files, 'gen', 'generated'), /locked/, 'a folder with a locked file is not moved');
refused(planFolderMove(files, 'assets', 'media'), /assets\/logo\.png is locked/, 'the locked file is named');
refused(planFolderMove(files, 'nope', 'x'), /not a folder/, 'a missing folder is refused');
refused(planFolderMove(files, 'ui', 'tessera.json'), /is a file/, 'a move onto a file is refused');
refused(planFolderMove(files, 'ui', 'tessera.json/x'), /inside a file/, 'a move inside a file is refused');
refused(planFolderMove(files, 'ui/parts', 'styles'.concat('/..', '/ui')), /./, 'a dotdot target is refused');
const clashing = [...files, { path: 'app/App.tsx', editable: true }];
refused(planFolderMove(clashing, 'ui', 'app'), /app\/App\.tsx already exists/, 'a collision names the path');

const del = planFolderDelete(files, 'ui');
check('a folder delete lists its files', del.ok && del.paths.join() === 'ui/App.tsx,ui/parts/Row.tsx');
refused(planFolderDelete(files, 'gen'), /locked/, 'a folder with a locked file is not deleted');
refused(planFolderDelete(files, 'zzz'), /not a folder/, 'a missing folder is refused');

console.log('preview_project_folder_ops: ' + checks + ' checks passed');
