-- Remove PKG artifacts through the admin API before rollback so their files
-- are cleaned up and published releases retain at least one installer.
ALTER TABLE release_artifacts
    DROP CONSTRAINT release_artifacts_kind_check,
    ADD CONSTRAINT release_artifacts_kind_check CHECK (kind IN (
        'windows-exe', 'macos-dmg', 'linux-appimage', 'linux-deb',
        'linux-rpm', 'linux-tar-gz', 'linux-tar-xz'));
