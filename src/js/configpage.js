/*
 * The settings page, built on the phone with Clay (https://github.com/pebble/clay) so it
 * doesn't need to be hosted anywhere. Based on the old hosted page in docs/index.html.
 *
 * Clay can't add form fields after the page is built, so the feed list is a fixed set of
 * slots: empty ones are hidden, and "Add feed" / "Remove last feed" show and hide them.
 * The app's feed list lives in Settings.option('feeds'); this module converts to and from
 * Clay's flat feedNameN / feedUrlN fields.
 *
 * In the emulator, Clay 1.0.4 points `pebble emu-app-config` at a page Pebble used to host on
 * clay.pebble.com, which no longer exists, so the page comes up blank there. Real phones get a
 * self-contained data: URL and aren't affected.
 */

var Clay = require('clay');

var MAX_FEEDS = 20;

function buildConfig() {
	var feedItems = [{
		type: 'heading',
		defaultValue: 'RSS Feeds'
	}];
	for (var i = 1; i <= MAX_FEEDS; i++) {
		feedItems.push({
			type: 'input',
			messageKey: 'feedName' + i,
			group: 'feed' + i,
			label: 'Feed ' + i,
			attributes: { maxlength: 16, placeholder: 'Name' }
		}, {
			type: 'input',
			messageKey: 'feedUrl' + i,
			group: 'feed' + i,
			label: 'URL',
			attributes: { type: 'url', placeholder: 'https://example.com/feed' }
		});
	}
	feedItems.push({
		type: 'button',
		id: 'addFeed',
		defaultValue: 'Add feed'
	}, {
		type: 'button',
		id: 'removeFeed',
		defaultValue: 'Remove last feed'
	});

	return [{
		type: 'section',
		items: feedItems
	}, {
		type: 'text',
		defaultValue: '<i>Your Pebble will only display content included in your RSS feed. If a feed only contains headlines, you will only be able to read headlines. You may be able to turn headline-only feeds into full-text feeds with fivefilters.org/content-only/.</i>'
	}, {
		type: 'submit',
		defaultValue: 'Save Changes'
	}];
}

/*
 * Runs inside the config page, not in the app: Clay serializes it with toSource, so it can't
 * use anything from this file's scope.
 */
function pageScript() {
	var clayConfig = this;

	clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function() {
		var slots = [];
		for (var n = 1; clayConfig.getItemByMessageKey('feedName' + n); n++) {
			slots.push(clayConfig.getItemsByGroup('feed' + n));
		}

		function isEmpty(slot) {
			return slot.every(function(item) { return !String(item.get() || '').trim(); });
		}
		function setVisible(slot, visible) {
			slot.forEach(function(item) { if (visible) { item.show(); } else { item.hide(); } });
		}

		// Show every slot up to the last one in use, and at least one.
		var visibleCount = 1;
		slots.forEach(function(slot, index) {
			if (!isEmpty(slot)) { visibleCount = index + 1; }
		});

		var addButton = clayConfig.getItemById('addFeed');
		var removeButton = clayConfig.getItemById('removeFeed');

		function refresh() {
			slots.forEach(function(slot, index) { setVisible(slot, index < visibleCount); });
			if (visibleCount < slots.length) { addButton.show(); } else { addButton.hide(); }
		}

		addButton.on('click', function() {
			if (visibleCount < slots.length) {
				visibleCount++;
				refresh();
			}
		});
		removeButton.on('click', function() {
			slots[visibleCount - 1].forEach(function(item) { item.set(''); });
			if (visibleCount > 1) {
				visibleCount--;
			}
			refresh();
		});

		refresh();
	});
}

var clay = new Clay(buildConfig(), pageScript, { autoHandleEvents: false });

/* A data: URL for the settings page, filled in with the given feeds. */
function url(feeds) {
	var values = {};
	for (var i = 1; i <= MAX_FEEDS; i++) {
		var feed = feeds[i - 1];
		values['feedName' + i] = feed ? feed.title : '';
		values['feedUrl' + i] = feed ? feed.url : '';
	}
	clay.setSettings(values);
	return clay.generateUrl();
}

/* The feeds in the page's response (Clay's { feedName1: { value: ... } } object). */
function feedsFromResponse(response) {
	var feeds = [];
	for (var i = 1; i <= MAX_FEEDS; i++) {
		var name = response['feedName' + i];
		var feedUrl = response['feedUrl' + i];
		var title = String((name && name.value) || '').trim();
		var address = String((feedUrl && feedUrl.value) || '').trim();
		if (title && address) {
			feeds.push({ title: title, url: address });
		}
	}
	return feeds;
}

module.exports = {
	url: url,
	feedsFromResponse: feedsFromResponse
};
