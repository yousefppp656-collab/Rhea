//! Deterministic package manager (`rhea.toml` & SHA-256 `rhea.lock` verification).
pub struct PackageManifest {
    pub name: String,
    pub version: String,
}
