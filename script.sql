USE mycal;

CREATE TABLE IF NOT EXISTS passwords (
    username VARCHAR(255) PRIMARY KEY,
    password_hash VARCHAR(255) NOT NULL
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS sessions (
    token_hash BINARY(32) PRIMARY KEY,
    username VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (username)
        REFERENCES passwords(username)
        ON DELETE CASCADE
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci;

INSERT INTO passwords (username, password_hash)
VALUES ('yop2', '$argon2id$v=19$m=65536,t=2,p=1$GK8CpyJq/A4G+PDUScuJOw$u2OwhPWH9xYEJSX2ci3lugfLlybWGVwdywAe7Z8fVdg');

    --('julien', '$argon2id$v=19$m=65536,t=2,p=1$myPr3NZ91cgIuPO8tBhpog$sh3uc25CONPaAYt7Eq5qWIK4OmTNLRSXLk0kipTVWrM'),
    --('elise', '$argon2id$v=19$m=65536,t=2,p=1$SgGytgzaqGQP/+eZXiyz0Q$++BUM3pZ0OIsjbPw1g1Tgo9ZnxXjC6ylHcbmFhPDuX8');




