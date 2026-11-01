# RC.2 follow-up: fresh desktop installation

RC.1 remains published at its original tag and bytes. Fresh company-style
installation checks found a real short-window layout defect on Windows and
macOS ARM, beyond the earlier successful upgrade checks. The database tree's
flex area could shrink to zero beneath the sidebar actions; a collection click
then hit “New database” or the search area instead.

RC.2 keeps a minimum usable database-tree area, prevents the other navigation
sections from shrinking over it, and permits the sidebar to scroll vertically.
The regression uses actual Workbench markup/CSS and ordinary pointer clicks,
without a server or forced clicks. It reproduces the original failure before
the fix and passes at1024x650,1024x653 and1360x900 afterwards. Existing installed
GUI tests now exercise both upgrade and a completely fresh data root.

Linux packaging now retains electron-builder's upstream post-install script and
its application-scoped AppArmor profile. RC.1's Ubuntu installer was built with
an explicit equivalent packaging override and passed its sandboxed installation
test; see [the original findings](../v1.2.0-rc.1/DESKTOP_INSTALLER_FINDINGS.md).
No global sandbox/AppArmor setting is disabled.

Base version remains1.2.0, candidate/tag becomes `v1.2.0-rc.2`. npm/Java versions
are1.2.0-rc.2 and Python is1.2.0rc2. The engine/storage/vector implementation tree
is identical to RC.1 commit `c553c6b6c62702f494f6f95acb7d53c6a410068e`;
Workbench layout/packaging, release metadata, tests and documentation differ.
Persistent storage format and Raft protocol remain2. This source equivalence
does not replace fresh package identity/runtime tests.

The RC.1 post-publication benchmark retains its actual artifact identity.
No RC.1 timing is relabeled as RC.2. RC.2 qualification, tag creation, publication
and new artifact benchmarks remain pending until their own receipts exist.
PyPI/Maven publication still requires owner setup. Production qualification
remains INCOMPLETE; the existing vector preview and distributed limits apply.
