-- MyFiles schema.
--
-- Every synced row carries:
--   created_at / updated_at  client clock, epoch ms (used for last-write-wins)
--   deleted                  tombstone, so deletions sync to other devices
--   rev                      server change counter; clients pull "rows with rev > cursor"

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE users (
  id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  email         text NOT NULL UNIQUE,
  password_hash text NOT NULL,
  created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE SEQUENCE sync_rev;

-- A physical file / envelope. id is the normalized file number, so two devices
-- that both create "F-0042" offline converge on the same row instead of clashing.
CREATE TABLE folders (
  user_id     uuid   NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  id          text   NOT NULL,
  file_number text   NOT NULL,
  title       text   NOT NULL DEFAULT '',
  location    text   NOT NULL DEFAULT '',  -- e.g. "Cabinet 2, drawer 3"
  notes       text   NOT NULL DEFAULT '',
  created_at  bigint NOT NULL,
  updated_at  bigint NOT NULL,
  deleted     boolean NOT NULL DEFAULT false,
  rev         bigint NOT NULL DEFAULT nextval('sync_rev'),
  PRIMARY KEY (user_id, id)
);
CREATE INDEX folders_rev ON folders (user_id, rev);

-- A logical document (bill, certificate, letter...) living inside a folder.
CREATE TABLE documents (
  user_id    uuid   NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  id         text   NOT NULL,
  folder_id  text   NOT NULL,
  title      text   NOT NULL DEFAULT '',
  tags       text   NOT NULL DEFAULT '',
  doc_date   text   NOT NULL DEFAULT '',   -- free-form / ISO date written on the document
  ocr_text   text   NOT NULL DEFAULT '',   -- concatenated text of all pages
  created_at bigint NOT NULL,
  updated_at bigint NOT NULL,
  deleted    boolean NOT NULL DEFAULT false,
  rev        bigint NOT NULL DEFAULT nextval('sync_rev'),
  search     tsvector GENERATED ALWAYS AS (
               setweight(to_tsvector('simple', coalesce(title, '')), 'A') ||
               setweight(to_tsvector('simple', coalesce(tags, '')), 'B') ||
               setweight(to_tsvector('simple', coalesce(ocr_text, '')), 'C')
             ) STORED,
  PRIMARY KEY (user_id, id)
);
CREATE INDEX documents_rev ON documents (user_id, rev);
CREATE INDEX documents_folder ON documents (user_id, folder_id);
CREATE INDEX documents_search ON documents USING gin (search);

-- One scanned page image of a document.
CREATE TABLE pages (
  user_id     uuid   NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  id          text   NOT NULL,
  document_id text   NOT NULL,
  page_no     integer NOT NULL DEFAULT 1,
  ocr_text    text   NOT NULL DEFAULT '',
  width       integer NOT NULL DEFAULT 0,
  height      integer NOT NULL DEFAULT 0,
  has_image   boolean NOT NULL DEFAULT false,  -- set by the server once the JPEG is uploaded
  created_at  bigint NOT NULL,
  updated_at  bigint NOT NULL,
  deleted     boolean NOT NULL DEFAULT false,
  rev         bigint NOT NULL DEFAULT nextval('sync_rev'),
  PRIMARY KEY (user_id, id)
);
CREATE INDEX pages_rev ON pages (user_id, rev);
CREATE INDEX pages_document ON pages (user_id, document_id);
