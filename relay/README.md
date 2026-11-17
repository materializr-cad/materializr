# Materializr report relay

A small Cloudflare Worker behind Help > Report a Bug. The app posts the report
here; the Worker stores the attachments and files the GitHub issue as the
`materializr-report` GitHub App. The app holds no credentials.

```
POST /v1/report        multipart: kind, title, description, env (JSON),
                       screenshot (PNG), log (text), project (.mzr, gzip)
GET  /f/<id>.png|.log  public screenshot / log (linked from the issue)
GET  /p/<id>           private project file, Authorization: Bearer <MAINTAINER_TOKEN>
```

## Limits (free-tier safe; change in `src/validate.ts` / `src/ratelimit.ts`)

| | |
|---|---|
| Screenshot / log / project | 3 MB / 600 KB / 8 MB |
| Per address | 10 reports an hour, 25 a day |
| Everyone | 100 reports a day, 80 MB of attachments a day (the byte budget is the real cost guard) |
| Retention | everything is deleted after **14 days** (R2 lifecycle rule `expire-all`) |

Counters live in one Durable Object (`Limiter`) so a burst cannot race past a
limit. At most 80 MB x 14 days = 1.1 GB is ever stored, far under R2's 10 GB
free tier. Workers, Durable Objects and R2 are all on their free tiers; none of
this should bill. Check usage in the Cloudflare dashboard if you raise a limit.

## Deploy

Credentials: `~/.config/materializr-relay/env` (`CLOUDFLARE_API_TOKEN`,
`CLOUDFLARE_ACCOUNT_ID`; create with `setup-cloudflare-env.sh`).

```sh
cd relay
npm install
npm test                      # unit + handler tests, plain node
npx tsc -p .                  # type check
set -a; . ~/.config/materializr-relay/env; set +a
npx wrangler deploy --env staging   # DRY_RUN: never touches GitHub
npx wrangler deploy                  # production
```

Secrets (`wrangler secret put <NAME> [--env staging]`): `GITHUB_APP_PRIVATE_KEY`
(the app's .pem, as downloaded), `MAINTAINER_TOKEN`, `IP_SALT`. The last two are
also kept in `~/.config/materializr-relay/{maintainer-token,ip-salt}`.

## Operating it

- **Download a private project:**
  `curl -H "Authorization: Bearer $(cat ~/.config/materializr-relay/maintainer-token)" https://materializr-relay.materializr-relay.workers.dev/p/<id> -o report.mzr`
  (the id is in the issue). Files are gone after 14 days.
- **Kill switch:** set the `DISABLED` var to `"1"` (dashboard or `wrangler.toml`) and
  redeploy; the app then falls back to its pre-filled GitHub link.
- **Rotate the GitHub App key:** generate a new private key on the app's settings
  page, `wrangler secret put GITHUB_APP_PRIVATE_KEY`, then delete the old key.
- **Rotate the maintainer token:** write a new random value to the file and
  `wrangler secret put MAINTAINER_TOKEN`.
- **GitHub App:** `materializr-report`, App ID 5806934, installation 183085462,
  permission Issues: read & write only, installed on `materializr-cad/materializr`.
