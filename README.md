![Banner](Banner.png)

# RSS Reader+

Hands full? No problem! As long as you have a Pebble smartwatch and at least one free finger, you can read the news—no pulling out your phone required.

_RSS Reader+_ lets you read complete news articles, blog posts, or anything else with an RSS or Atom feed on your Pebble smartwatch. An unlimited number of feeds are supported, as are articles of any length. Peruse the default feeds, or add your own in Settings.

## What's new compared to Pebble RSS Reader

RSS Reader+ is a continuation of [Pebble RSS Reader](https://github.com/Wowfunhappy/Pebble-RSS-Reader) by Jonathan Alland (Wowfunhappy), which hasn't been updated in years.

- Builds with the current Pebble SDK and works with the current Pebble phone app.
- Feeds are downloaded and read directly on your phone, with no third-party service in between (so no rss2json account or rate limits).
- Article pages are sized by the watch itself, so text isn't cut off on round watches and fills the screen on larger ones.
- The settings page is built into the app, and can import and export your feeds as OPML.
- Long article titles scroll so you can read them in full, the feed list can wrap around at either end, and round watches show the feed name above its articles.
- Fixes for a crash when returning to the article list, and for getting stuck on "Loading..." when a feed can't be read.

## Development

This is a PebbleJS project, so most of the code specific to this app is in [src/js/app.js](src/js/app.js), with the settings page in [src/js/configpage.js](src/js/configpage.js) and feed parsing in [src/js/lib/feedparser.js](src/js/lib/feedparser.js). Build it with the [Pebble SDK](https://developer.repebble.com/sdk/) using `pebble build`.

The code is licensed under the GPL (v3 or later); see [LICENSE](LICENSE). The settings page uses [Clay](https://github.com/pebble/clay) (MIT).

## Thanks

- Jonathan Alland (Wowfunhappy), who wrote Pebble RSS Reader
- Molayl (rss2json api), which the original app used to read feeds
- Rebble Team

...and every website that offers full-text RSS feeds
