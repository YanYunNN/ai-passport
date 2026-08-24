-- Keep 0002 immutable for deployments that have already applied it.
-- Store both Cloudflare Access email and subject for an approval identity.
ALTER TABLE device_enrollments ADD COLUMN approved_subject TEXT;
