# Getting a Google Drive refresh token for Desk Buddy

The device uploads segments straight to your Drive. It needs three strings, entered once in
**Settings** in the web app (or on the setup portal) and then stored in NVS:

| Field | Where it comes from |
|---|---|
| `drive_client_id` | step 4 |
| `drive_client_secret` | step 4 |
| `drive_refresh_token` | step 7 |

Everything below is a one-time, ~10 minute job. There is no backend and no service account:
the device holds your refresh token and talks to Drive directly.

You need: a Google account, a browser, and `curl` (Git Bash, WSL, macOS, Linux — or
`curl.exe` which ships with Windows 10/11). Commands are shown for a POSIX shell; a PowerShell
version of the only tricky command is at the end.

---

## 1. Create a Google Cloud project

Browser: <https://console.cloud.google.com/projectcreate>

- **Project name:** `deskbuddy` (anything works)
- Click **Create**, wait for the notification, then select the project in the top bar.

Or with the `gcloud` CLI if you have it:

```sh
gcloud projects create deskbuddy-$RANDOM --name=deskbuddy
gcloud config set project <the-project-id-it-printed>
```

## 2. Enable the Drive API

Browser: <https://console.cloud.google.com/apis/library/drive.googleapis.com> → **Enable**.

```sh
gcloud services enable drive.googleapis.com
```

Nothing else needs enabling. Do not enable billing; this stays inside the free quota.

## 3. Configure the OAuth consent screen

Browser: <https://console.cloud.google.com/auth/overview> (older UI:
*APIs & Services → OAuth consent screen*).

1. **User type: External.** ("Internal" only exists for Workspace orgs.)
2. App name `deskbuddy`, your email for both support and developer contact. Save.
3. **Scopes:** add `https://www.googleapis.com/auth/drive.file` and nothing else.
   That scope lets the app touch *only files it created itself* — it cannot read the rest of
   your Drive. This is deliberate; do not widen it.
4. **Test users:** add your own Google account address.

> ### Read this or your uploads stop after a week
> While the app is in **Testing**, Google expires refresh tokens after **7 days**.
> Go to **Audience** → **Publish app** and confirm. You will see an "unverified app" warning
> during step 5 — that is expected and fine for a personal app requesting only `drive.file`;
> verification is only required to remove the warning for other people. A published app's
> refresh token does not expire on a timer.

## 4. Create an OAuth client ID

Browser: <https://console.cloud.google.com/auth/clients> → **Create client**.

- **Application type: Desktop app**
- Name: `deskbuddy-device`
- **Create**, then copy the **Client ID** and **Client secret**.

Desktop clients are allowed to use the loopback redirect (`http://localhost`), which is what
makes step 5 possible without hosting anything. Keep the secret out of git; it goes into NVS
on the device, nowhere else.

Export them so the rest of this page is copy-paste:

```sh
export CLIENT_ID='8123-abc.apps.googleusercontent.com'
export CLIENT_SECRET='GOCSPX-xxxxxxxxxxxxxxxx'
```

## 5. Get an authorization code

Build the consent URL:

```sh
echo "https://accounts.google.com/o/oauth2/v2/auth\
?client_id=$CLIENT_ID\
&redirect_uri=http://localhost:8080\
&response_type=code\
&scope=https://www.googleapis.com/auth/drive.file\
&access_type=offline\
&prompt=consent"
```

`access_type=offline` is what makes Google issue a refresh token at all, and `prompt=consent`
forces a fresh one even if you have authorised this client before. Without both you get an
access token that dies in an hour and no way to renew it.

Open that URL in your browser, pick your account, click through the "Google hasn't verified
this app" warning (**Advanced → Go to deskbuddy (unsafe)**), and **Allow**.

The browser then lands on `http://localhost:8080/?code=...` and shows **"This site can't be
reached"**. That is the success case — nothing is listening on port 8080 and nothing needs to
be. Copy the `code` value out of the address bar.

It is URL-encoded (`%2F` for `/`). Copy it exactly as shown, including the `4/0A...` prefix:

```sh
export CODE='4%2F0AVMBsJi...paste-the-whole-thing...'
```

The code is valid for a few minutes and single-use. If step 6 fails with `invalid_grant`, just
reload the consent URL and get a new one.

## 6. Exchange the code for a refresh token

```sh
curl -s https://oauth2.googleapis.com/token \
  -d client_id="$CLIENT_ID" \
  -d client_secret="$CLIENT_SECRET" \
  -d code="$CODE" \
  -d grant_type=authorization_code \
  -d redirect_uri=http://localhost:8080
```

Response:

```json
{
  "access_token": "ya29.a0Af...",
  "expires_in": 3599,
  "refresh_token": "1//0gABCDEF...",
  "scope": "https://www.googleapis.com/auth/drive.file",
  "token_type": "Bearer"
}
```

**`refresh_token` is the one you want.** Ignore `access_token` — the device mints its own from
the refresh token on every boot and caches it until it expires.

If `refresh_token` is missing from the response, you skipped `access_type=offline` or
`prompt=consent` in step 5. Redo step 5.

## 7. Verify it works before touching the device

Mint an access token from the refresh token, exactly the way the firmware does:

```sh
export REFRESH_TOKEN='1//0gABCDEF...'

curl -s https://oauth2.googleapis.com/token \
  -d client_id="$CLIENT_ID" \
  -d client_secret="$CLIENT_SECRET" \
  -d refresh_token="$REFRESH_TOKEN" \
  -d grant_type=refresh_token
```

You should get an `access_token` back and **no** `refresh_token` (that is normal — you keep
the one you already have). Now prove the scope works:

```sh
export ACCESS_TOKEN='ya29....'

# create the folder the device will use
curl -s -X POST 'https://www.googleapis.com/drive/v3/files?fields=id,name' \
  -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"name":"DeskBuddy","mimeType":"application/vnd.google-apps.folder"}'
```

Check <https://drive.google.com> — a `DeskBuddy` folder should be there. The device will find
and reuse this folder rather than creating a second one.

If you get `403 insufficientPermissions`, the scope in step 3 is wrong.
If you get `401`, the access token expired (they last an hour) — mint another.

## 8. Put the three values on the device

Web app → **Settings** → Drive section → paste `drive_client_id`, `drive_client_secret`,
`drive_refresh_token` → **Save** → **Reboot**.

Or over HTTP:

```sh
curl -X POST http://<device-ip>/api/settings \
  -H "X-Device-Pin: <your-pin>" \
  -H 'Content-Type: application/json' \
  -d "{\"drive_client_id\":\"$CLIENT_ID\",\"drive_client_secret\":\"$CLIENT_SECRET\",\"drive_refresh_token\":\"$REFRESH_TOKEN\"}"

curl -X POST http://<device-ip>/api/reboot -H "X-Device-Pin: <your-pin>"
```

Then confirm — `GET /api/settings` must come back **masked**:

```json
{ "drive_client_id": "8123-abc.apps.googleusercontent.com",
  "drive_client_secret": "********3456",
  "drive_refresh_token": "********CDEF" }
```

If a secret comes back in full, that is a T8 failure — stop and report it.

Within one segment rotation (5 minutes) plus upload time, files start appearing in
`DeskBuddy/YYYYMMDD/`.

### Optional: upload into a folder you already have

Set `drive_folder_id` as well and the device creates `DeskBuddy/` *inside* that folder instead
of at your Drive root. The id is the last path element of the folder's URL:
`https://drive.google.com/drive/folders/`**`1AbCdEfGhIjKlMnOp`**.

Because the scope is `drive.file`, the device can only write into a folder **it created**, or
one you explicitly shared with this OAuth client. If uploads fail with `404` on the parent,
leave `drive_folder_id` empty and let it create `DeskBuddy` at the root.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `invalid_grant` in step 6 | Code already used, expired, or `redirect_uri` differs from step 5 | Redo step 5, use the code within a minute, keep `http://localhost:8080` identical in both |
| No `refresh_token` in the response | Missing `access_type=offline` / `prompt=consent` | Redo step 5 with both |
| `redirect_uri_mismatch` | The client is not type **Desktop app** | Recreate the client as Desktop app (step 4) |
| Uploads work, then stop after ~7 days | Consent screen still in **Testing** | Publish the app (step 3), then redo steps 5–6 for a fresh token |
| `drive auth failed (400)` on serial | Client id/secret/refresh token mismatched | Re-run step 7's first command locally; if it fails there, the values are wrong |
| `drive auth failed (401)` on serial | Refresh token revoked | Revoke and reissue: <https://myaccount.google.com/permissions>, then redo steps 5–6 |
| Files land in the wrong account | You consented with a different Google login | Redo step 5 in a private window, choose the right account |

## Revoking

If the device is ever lost or you want to cut it off:
<https://myaccount.google.com/permissions> → **deskbuddy** → **Remove access**.
The refresh token dies immediately. Already-uploaded files stay in your Drive.

---

## PowerShell equivalents

Only the `curl` calls differ. Use `curl.exe` explicitly — bare `curl` in PowerShell is an
alias for `Invoke-WebRequest` and takes different arguments.

```powershell
$CLIENT_ID     = '8123-abc.apps.googleusercontent.com'
$CLIENT_SECRET = 'GOCSPX-xxxxxxxxxxxxxxxx'
$CODE          = '4%2F0AVMBsJi...'

curl.exe -s https://oauth2.googleapis.com/token `
  -d client_id=$CLIENT_ID `
  -d client_secret=$CLIENT_SECRET `
  -d code=$CODE `
  -d grant_type=authorization_code `
  -d redirect_uri=http://localhost:8080
```
