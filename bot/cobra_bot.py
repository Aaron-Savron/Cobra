"""Cobra community Discord bot.

Required environment variables:
  DISCORD_BOT_TOKEN
  COBRA_SITE_URL (optional, defaults to the GitHub Pages site)
  GITHUB_REPO (optional, defaults to Aaron-Savron/Cobra)
  DISCORD_GUILD_ID (optional, syncs commands immediately to one guild)
"""

from __future__ import annotations

import os
from urllib.parse import quote

import discord
from discord import app_commands
from discord.ext import commands


TOKEN = os.environ["DISCORD_BOT_TOKEN"]
REPO = os.getenv("GITHUB_REPO", "Aaron-Savron/Cobra")
SITE = os.getenv("COBRA_SITE_URL", f"https://{REPO.split('/')[0].lower()}.github.io/Cobra/")


class CobraBot(commands.Bot):
    def __init__(self) -> None:
        intents = discord.Intents.default()
        super().__init__(command_prefix="!", intents=intents)

    async def setup_hook(self) -> None:
        guild_id = os.getenv("DISCORD_GUILD_ID")
        if guild_id:
            guild = discord.Object(id=int(guild_id))
            self.tree.copy_global_to(guild=guild)
            await self.tree.sync(guild=guild)
        else:
            await self.tree.sync()

    async def on_ready(self) -> None:
        print(f"Connected as {self.user}")


bot = CobraBot()


def is_staff(interaction: discord.Interaction) -> bool:
    return bool(interaction.user.guild_permissions.manage_guild)


@bot.tree.command(description="Open the Cobra documentation")
async def docs(interaction: discord.Interaction) -> None:
    await interaction.response.send_message(f"Cobra docs: {SITE}", ephemeral=True)


@bot.tree.command(description="Show the main Cobra help links")
async def help(interaction: discord.Interaction) -> None:
    message = (
        f"Docs: {SITE}\n"
        f"Source: https://github.com/{REPO}\n"
        f"Issues: https://github.com/{REPO}/issues\n"
        f"Roadmap: https://github.com/{REPO}/blob/main/ROADMAP.md"
    )
    await interaction.response.send_message(message, ephemeral=True)


@bot.tree.command(description="Open the Cobra roadmap")
async def roadmap(interaction: discord.Interaction) -> None:
    await interaction.response.send_message(
        f"Cobra roadmap: https://github.com/{REPO}/blob/main/ROADMAP.md", ephemeral=True
    )


@bot.tree.command(description="Post a release update in the releases channel")
@app_commands.describe(commit="Commit message or release summary", url="Optional GitHub URL")
async def announce(interaction: discord.Interaction, commit: str, url: str = "") -> None:
    if not is_staff(interaction):
        await interaction.response.send_message("Only server staff can announce releases.", ephemeral=True)
        return

    channel_id = os.getenv("DISCORD_RELEASE_CHANNEL_ID")
    if not channel_id:
        await interaction.response.send_message("DISCORD_RELEASE_CHANNEL_ID is not configured.", ephemeral=True)
        return
    channel = bot.get_channel(int(channel_id))
    if not isinstance(channel, discord.TextChannel):
        await interaction.response.send_message("The configured release channel could not be found.", ephemeral=True)
        return

    embed = discord.Embed(
        title="Cobra release update",
        description=commit[:4000],
        url=url or f"https://github.com/{REPO}/commits/main",
        color=0xC8A44A,
    )
    embed.add_field(name="Author", value=interaction.user.mention, inline=True)
    embed.add_field(name="Repository", value=f"[{REPO}](https://github.com/{REPO})", inline=True)
    embed.set_footer(text="Cobra repository")
    await channel.send(embed=embed)
    await interaction.response.send_message("Release update posted.", ephemeral=True)


@bot.tree.command(description="Open a prefilled Cobra issue form")
@app_commands.describe(summary="Short description of the bug or idea")
async def issue(interaction: discord.Interaction, summary: str) -> None:
    title = quote(summary[:90])
    url = f"https://github.com/{REPO}/issues/new?title={title}"
    await interaction.response.send_message(f"Open a GitHub issue: {url}", ephemeral=True)


if __name__ == "__main__":
    bot.run(TOKEN)
