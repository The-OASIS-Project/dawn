# Google OAuth 2.0 Setup for DAWN

This guide walks through setting up Google OAuth for Calendar and Email integration.

## 1. Create a Google Cloud Project

1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Click **New Project** in the top bar
3. Name it (e.g., "DAWN Assistant") and click **Create**

## 2. Enable APIs

1. Go to **APIs & Services > Library**
2. Search for and enable:
   - **CalDAV API** (required for calendar sync)
   - **Gmail API** (required for email integration — DAWN reads and sends Gmail through it)

## 3. Configure OAuth Consent Screen

1. Go to **APIs & Services > OAuth consent screen**
2. Select **External** user type (unless you have a Google Workspace org)
3. Fill in:
   - **App name**: DAWN Assistant
   - **User support email**: your email
   - **Developer contact**: your email
4. Add scopes:
   - `https://www.googleapis.com/auth/calendar` (Calendar read/write)
   - `https://mail.google.com/` (Email — full IMAP/SMTP access via XOAUTH2)
5. Add **test users** (your Google account email)
   - In "Testing" mode, only listed test users can authorize
   - Up to 100 test users allowed without Google verification
6. Click **Save**

## 4. Create OAuth Credentials

1. Go to **APIs & Services > Credentials**
2. Click **Create Credentials > OAuth client ID**
3. Application type: **Web application**
4. Name: "DAWN Web Client"
5. Add **Authorized redirect URIs**:
   ```
   https://<your-fqdn>:3000/oauth/callback
   ```

   **Google requires a fully qualified domain name (FQDN)** — bare IP addresses and
   `localhost` are not accepted for production OAuth clients. If your DAWN server is on a
   local network, create a DNS A record pointing a subdomain to your server's local IP.
   For example:

   | Host    | Type | Value         |
   |---------|------|---------------|
   | jetson  | A    | 192.168.1.159 |

   Then use `https://jetson.yourdomain.com:3000/oauth/callback` as the redirect URI.

   **Important**: This URI must exactly match the `redirect_url` you set in `secrets.toml`
   (step 5). Google rejects requests where they differ, even by a trailing slash.
6. Click **Create**
7. Copy the **Client ID** and **Client Secret**

## 5. Configure DAWN

Add credentials to `secrets.toml`:

```toml
[secrets.google]
client_id = "123456789-abc.apps.googleusercontent.com"
client_secret = "GOCSPX-..."
redirect_url = "https://jetson.yourdomain.com:3000/oauth/callback"
```

The `redirect_url` **must exactly match** the redirect URI you added in Google Cloud Console
(step 4). Use the same FQDN you configured in your DNS A record (see step 4).

Or use environment variables:

```bash
export DAWN_GOOGLE_CLIENT_ID="123456789-abc.apps.googleusercontent.com"
export DAWN_GOOGLE_CLIENT_SECRET="GOCSPX-..."
export DAWN_GOOGLE_REDIRECT_URL="https://jetson.yourdomain.com:3000/oauth/callback"
```

Restart the DAWN daemon after configuration.

## 6. Add a Google Calendar Account

1. Open the DAWN WebUI
2. Go to **Settings > Calendar Accounts**
3. Click **Add Account**
4. Select **Google OAuth** (tab at top)
5. Enter an account name (e.g., "Google Calendar")
6. Click **Connect with Google**
7. A popup will open with Google's consent screen
8. Sign in and authorize DAWN
9. The popup will close automatically
10. Click **Save Account**

## Multiple Google Accounts

You can connect multiple Google accounts for both Calendar and Email. Each "Add Account" +
"Connect with Google" click opens a fresh OAuth popup where you can pick a different Google
account. Calendar and email use **separate token sets** — connecting a second Gmail account
for email does not affect existing calendar tokens, and vice versa.

## Troubleshooting

### "redirect_uri_mismatch" Error
Three things must match exactly:
1. The redirect URI in **Google Cloud Console** (step 4)
2. The `redirect_url` in **secrets.toml** (step 5)
3. The URL you use to **access the WebUI** in your browser

Check that the FQDN, port, and protocol (`https`) are identical across all three.
Remember that Google requires an FQDN — bare IP addresses won't work. If you're on a
local network, set up a DNS A record pointing to your server's LAN IP (see step 4).

### "App not verified" Warning
This is normal for apps in "Testing" mode. Click **Advanced > Go to DAWN (unsafe)**
to proceed. Only test users added in step 3 can authorize.

### Popup Blocked
If your browser blocks the popup, allow popups for your DAWN WebUI URL.
The UI will show a message if the popup was blocked.

### Scopes Not Enabled
Make sure the **CalDAV API** is enabled in your project (step 2). Note: this is different from the "Google Calendar API".

### Token Expired / Revoked
Tokens auto-refresh. If refresh fails (e.g., you revoked access at
[myaccount.google.com/permissions](https://myaccount.google.com/permissions)),
remove and re-add the account.

## 7. Add a Google Email Account (Optional)

Calendar and email use **separate OAuth token sets** — revoking one does not affect the other.

1. Build DAWN with email enabled: `cmake --preset debug -DDAWN_ENABLE_EMAIL_TOOL=ON`
2. Open the DAWN WebUI
3. Go to **Settings > Email Accounts**
4. Click **Add Account**
5. Select **Google OAuth** (tab at top)
6. Enter an account name (e.g., "Gmail")
7. Click **Connect with Google**
8. Authorize DAWN (consent screen will request mail access)
9. The popup will close automatically
10. Click **Save Account**

A Google OAuth account is read and sent through the Gmail API — no app password needed for Google accounts.
