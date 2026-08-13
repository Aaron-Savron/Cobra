# Cobra Discord Bot

The bot provides small, useful links for the Cobra community:

- `/docs`
- `/help`
- `/roadmap`
- `/issue summary`
- `/announce commit url` for staff release notes

Push announcements are sent automatically by `.github/workflows/discord-release.yml`.
Add `DISCORD_BOT_TOKEN` and `DISCORD_RELEASE_CHANNEL_ID` as GitHub Actions secrets. The channel ID should point to `#releases`.

## Run locally

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r bot/requirements.txt
set -a; . bot/.env; set +a
python3 bot/cobra_bot.py
```

The bot token must stay in the environment. Do not commit `.env` files.
