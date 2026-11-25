/*
 * The settings page, built on the phone with Clay (https://github.com/pebble/clay) so it
 * doesn't need to be hosted anywhere.
 *
 * Clay can't add form fields after the page is built, so the feed list is a set of slots
 * sized when the page opens (the saved feeds plus some spare): empty ones are hidden, and
 * "Add feed" / "Remove last feed" show and hide them.
 *
 * OPML import/export uses plain text boxes, which work in every phone's web view (file pickers
 * and downloads depend on the phone app). Imported feeds aren't put into slots, so there's no
 * limit on how many: the page parses the OPML for feedback, passes the feeds back in a hidden
 * field, and they're merged into the list on save, skipping URLs that are already there.
 *
 * The app's feed list lives in Settings.option('feeds'); this module converts to and from
 * Clay's flat feedNameN / feedUrlN fields.
 *
 * In the emulator, Clay 1.0.4 points `pebble emu-app-config` at a page Pebble used to host on
 * clay.pebble.com, which no longer exists, so the page comes up blank there. Real phones get a
 * self-contained data: URL and aren't affected.
 */

var Clay = require('clay');

var MIN_SLOTS = 20;
var SPARE_SLOTS = 10;

var TEXTAREA_STYLE = 'display:block;width:100%;box-sizing:border-box;min-height:7em;margin-top:0.5em;' +
	'padding:0.5em;border:none;border-radius:0.25rem;background:#414141;color:#fff;' +
	'font-family:monospace;font-size:12px;';

function buildConfig(slotCount) {
	var feedItems = [{
		type: 'heading',
		defaultValue: 'RSS Feeds'
	}];
	for (var i = 1; i <= slotCount; i++) {
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

	var opmlItems = [{
		type: 'heading',
		defaultValue: 'Import / Export (OPML)'
	}, {
		type: 'text',
		defaultValue: 'To import feeds from another reader, paste its OPML export here. ' +
			'They\'ll be added to your feeds when you save; ones you already have are skipped.' +
			'<textarea id="opml-import" style="' + TEXTAREA_STYLE + '" placeholder="Paste OPML here" ' +
			'autocapitalize="off" autocorrect="off" spellcheck="false"></textarea>' +
			'<span id="opml-import-status"></span>'
	}, {
		// Carries the parsed import back to the app; hidden once the page is built.
		type: 'input',
		messageKey: 'opmlImport',
		label: 'Import data'
	}, {
		type: 'text',
		defaultValue: 'To export, copy this OPML into a file or another reader:' +
			'<textarea id="opml-export" style="' + TEXTAREA_STYLE + '" readonly></textarea>'
	}, {
		type: 'button',
		id: 'copyOpml',
		defaultValue: 'Copy OPML'
	}];

	var optionItems = [{
		type: 'heading',
		defaultValue: 'Options'
	}, {
		type: 'radiogroup',
		messageKey: 'theme',
		label: 'Colors',
		defaultValue: 'light',
		options: [
			{ label: 'Black text on white', value: 'light' },
			{ label: 'White text on black', value: 'dark' }
		]
	}, {
		type: 'toggle',
		messageKey: 'wrapFeedList',
		label: 'Wrap around the feed list',
		description: 'Pressing up on the first feed goes to the last one, and down on the last goes to the first.',
		defaultValue: true
	}, {
		type: 'radiogroup',
		messageKey: 'titleScroll',
		label: 'Scroll long titles',
		description: 'How the highlighted title in a list scrolls when it\'s too long to show in full.',
		defaultValue: 'relaxed',
		options: [
			{ label: 'Relaxed: wait 2 seconds before scrolling, and pause at the end', value: 'relaxed' },
			{ label: 'Quick: start after 1 second', value: 'quick' },
			{ label: 'Off', value: 'off' }
		]
	}, {
		type: 'radiogroup',
		messageKey: 'titleScrollSpeed',
		label: 'Title scrolling speed',
		description: 'How fast a long title scrolls once it starts.',
		defaultValue: '1',
		options: [
			{ label: '1x', value: '1' },
			{ label: '1.5x', value: '1.5' },
			{ label: '2x', value: '2' }
		]
	}, {
		type: 'radiogroup',
		messageKey: 'readingProgress',
		label: 'Reading progress',
		description: 'How far through an article you are, shown at the top of its pages.',
		defaultValue: 'bar',
		options: [
			{ label: 'Bar under the clock', value: 'bar' },
			{ label: 'Percentage instead of the clock', value: 'percent' },
			{ label: 'Off', value: 'off' }
		]
	}, {
		type: 'toggle',
		messageKey: 'shrinkParagraphEnds',
		label: 'Shrink text to finish a paragraph',
		description: 'When the end of a paragraph just misses fitting on a page, show it in smaller text ' +
			'rather than putting the last few words on a page of their own. Off: text is never shrunk.',
		defaultValue: true
	}];

	return [{
		type: 'section',
		items: feedItems
	}, {
		type: 'section',
		items: optionItems
	}, {
		type: 'section',
		items: opmlItems
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
			slots.push({
				name: clayConfig.getItemByMessageKey('feedName' + n),
				url: clayConfig.getItemByMessageKey('feedUrl' + n),
				items: clayConfig.getItemsByGroup('feed' + n)
			});
		}

		function value(item) {
			return String(item.get() || '').trim();
		}
		function isEmpty(slot) {
			return !value(slot.name) && !value(slot.url);
		}
		function feedKey(url) {
			return url.trim().toLowerCase().replace(/^https?:\/\//, '').replace(/\/+$/, '');
		}
		function currentFeeds() {
			var feeds = [];
			slots.forEach(function(slot) {
				if (value(slot.name) && value(slot.url)) {
					feeds.push({ title: value(slot.name), url: value(slot.url) });
				}
			});
			return feeds;
		}

		/* ---- Feed slots ---- */

		// Show every slot up to the last one in use, and at least one.
		var visibleCount = 1;
		slots.forEach(function(slot, index) {
			if (!isEmpty(slot)) { visibleCount = index + 1; }
		});

		var addButton = clayConfig.getItemById('addFeed');
		var removeButton = clayConfig.getItemById('removeFeed');

		function refreshSlots() {
			slots.forEach(function(slot, index) {
				slot.items.forEach(function(item) {
					if (index < visibleCount) { item.show(); } else { item.hide(); }
				});
			});
			if (visibleCount < slots.length) { addButton.show(); } else { addButton.hide(); }
		}

		addButton.on('click', function() {
			if (visibleCount < slots.length) {
				visibleCount++;
				refreshSlots();
			}
		});
		removeButton.on('click', function() {
			slots[visibleCount - 1].items.forEach(function(item) { item.set(''); });
			if (visibleCount > 1) {
				visibleCount--;
			}
			refreshSlots();
			refreshExport();
		});

		/* ---- OPML export ---- */

		var exportBox = document.getElementById('opml-export');

		function escapeXml(text) {
			return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
				.replace(/"/g, '&quot;');
		}
		function refreshExport() {
			var lines = [
				'<?xml version="1.0" encoding="UTF-8"?>',
				'<opml version="2.0">',
				'  <head><title>RSS Reader+ feeds</title></head>',
				'  <body>'
			];
			currentFeeds().forEach(function(feed) {
				var title = escapeXml(feed.title);
				lines.push('    <outline type="rss" text="' + title + '" title="' + title +
					'" xmlUrl="' + escapeXml(feed.url) + '"/>');
			});
			lines.push('  </body>', '</opml>');
			exportBox.value = lines.join('\n');
		}

		var copyButton = clayConfig.getItemById('copyOpml');
		copyButton.on('click', function() {
			function report(copied) {
				copyButton.set(copied ? 'Copied!' : 'Select the text above and copy it');
				setTimeout(function() { copyButton.set('Copy OPML'); }, 2500);
			}
			function selectAndCopy() {
				exportBox.focus();
				exportBox.select();
				exportBox.setSelectionRange(0, exportBox.value.length);
				try {
					return document.execCommand('copy');
				} catch (e) {
					return false;
				}
			}
			if (navigator.clipboard && window.isSecureContext) {
				navigator.clipboard.writeText(exportBox.value).then(function() {
					report(true);
				}, function() {
					report(selectAndCopy());
				});
			} else {
				report(selectAndCopy());
			}
		});

		/* ---- Title scrolling speed: only shown while titles scroll ---- */

		var scrollItem = clayConfig.getItemByMessageKey('titleScroll');
		var speedItem = clayConfig.getItemByMessageKey('titleScrollSpeed');
		function showScrollSpeed() {
			if (scrollItem.get() === 'off') { speedItem.hide(); } else { speedItem.show(); }
		}
		scrollItem.on('change', showScrollSpeed);
		showScrollSpeed();

		/* ---- OPML import ---- */

		var importBox = document.getElementById('opml-import');
		var importStatus = document.getElementById('opml-import-status');
		var importItem = clayConfig.getItemByMessageKey('opmlImport');
		importItem.hide();
		importItem.set('');

		function attribute(element, name) {
			// Case-insensitive: exporters disagree on xmlUrl vs xmlurl.
			for (var i = 0; i < element.attributes.length; i++) {
				if (element.attributes[i].name.toLowerCase() === name) {
					return element.attributes[i].value.trim();
				}
			}
			return '';
		}
		function parseOpml(text) {
			var doc = new DOMParser().parseFromString(text, 'text/xml');
			if (doc.getElementsByTagName('parsererror').length) {
				// Not well-formed XML (some exporters don't escape "&"): read it leniently instead.
				doc = new DOMParser().parseFromString(text, 'text/html');
			}
			var outlines = doc.getElementsByTagName('outline');
			var feeds = [];
			for (var i = 0; i < outlines.length; i++) {
				// Folders are outlines without an xmlUrl; their feeds are flattened into one list.
				var url = attribute(outlines[i], 'xmlurl');
				if (url) {
					feeds.push({
						title: attribute(outlines[i], 'title') || attribute(outlines[i], 'text') || url,
						url: url
					});
				}
			}
			return feeds;
		}
		function refreshImport() {
			var text = importBox.value.trim();
			if (!text) {
				importItem.set('');
				importStatus.textContent = '';
				return;
			}
			var feeds = parseOpml(text);
			importItem.set(feeds.length ? JSON.stringify(feeds) : '');
			if (!feeds.length) {
				importStatus.textContent = 'No feeds found. Paste the whole OPML file, including the <outline> lines.';
				return;
			}
			var known = {};
			currentFeeds().forEach(function(feed) { known[feedKey(feed.url)] = true; });
			var added = 0;
			feeds.forEach(function(feed) {
				var key = feedKey(feed.url);
				if (!known[key]) {
					known[key] = true;
					added++;
				}
			});
			var found = 'Found ' + feeds.length + (feeds.length === 1 ? ' feed' : ' feeds');
			if (added === 0) {
				importStatus.textContent = found + ', but you already have ' +
					(feeds.length === 1 ? 'it' : 'all of them') + '.';
			} else {
				importStatus.textContent = found + (added === feeds.length ? '' : ' (' + added + ' new)') +
					'. Save to add ' + (added === 1 ? 'it' : 'them') + '.';
			}
		}

		importBox.addEventListener('input', refreshImport);

		// Keep the export (and the import's "new" count) in step with edits to the feed list.
		document.addEventListener('input', function(e) {
			if (e.target !== importBox && e.target !== exportBox) {
				refreshExport();
				refreshImport();
			}
		}, true);

		refreshSlots();
		refreshExport();
	});
}

function feedKey(url) {
	return String(url).trim().toLowerCase().replace(/^https?:\/\//, '').replace(/\/+$/, '');
}

/*
 * A data: URL for the settings page, filled in with the given settings
 * ({ feeds, wrapFeedList, titleScroll, titleScrollSpeed, readingProgress, shrinkParagraphEnds,
 * theme }).
 */
function url(settings) {
	var feeds = settings.feeds;
	var slotCount = Math.max(MIN_SLOTS, feeds.length + SPARE_SLOTS);
	var values = {
		opmlImport: '',
		wrapFeedList: settings.wrapFeedList,
		titleScroll: settings.titleScroll,
		titleScrollSpeed: settings.titleScrollSpeed,
		readingProgress: settings.readingProgress,
		shrinkParagraphEnds: settings.shrinkParagraphEnds,
		theme: settings.theme
	};
	for (var i = 1; i <= slotCount; i++) {
		var feed = feeds[i - 1];
		values['feedName' + i] = feed ? feed.title : '';
		values['feedUrl' + i] = feed ? feed.url : '';
	}
	var clay = new Clay(buildConfig(slotCount), pageScript, { autoHandleEvents: false });
	clay.setSettings(values);
	return clay.generateUrl();
}

/*
 * The settings in the page's response (Clay's { feedName1: { value: ... } } object):
 * { feeds, wrapFeedList, titleScroll, titleScrollSpeed, readingProgress, shrinkParagraphEnds,
 * theme }. The feeds are the slots in order, then any imported feeds whose URLs aren't already in the list.
 */
function settingsFromResponse(response) {
	var wrap = response.wrapFeedList;
	var scroll = response.titleScroll && response.titleScroll.value;
	var speed = response.titleScrollSpeed && response.titleScrollSpeed.value;
	var progress = response.readingProgress && response.readingProgress.value;
	var shrink = response.shrinkParagraphEnds;
	var theme = response.theme && response.theme.value;
	return {
		feeds: feedsFromResponse(response),
		wrapFeedList: wrap && typeof wrap.value === 'boolean' ? wrap.value : true,
		titleScroll: (scroll === 'quick' || scroll === 'off') ? scroll : 'relaxed',
		titleScrollSpeed: (speed === '1.5' || speed === '2') ? speed : '1',
		readingProgress: (progress === 'percent' || progress === 'off') ? progress : 'bar',
		shrinkParagraphEnds: shrink && typeof shrink.value === 'boolean' ? shrink.value : true,
		theme: theme === 'dark' ? 'dark' : 'light'
	};
}

function feedsFromResponse(response) {
	function value(key) {
		var field = response[key];
		return String((field && field.value) || '').trim();
	}

	var feeds = [];
	var known = {};
	function add(title, address) {
		var key = feedKey(address);
		if (title && address && !known[key]) {
			known[key] = true;
			feeds.push({ title: title, url: address });
		}
	}

	for (var i = 1; response.hasOwnProperty('feedName' + i); i++) {
		add(value('feedName' + i), value('feedUrl' + i));
	}

	var imported = [];
	try {
		imported = JSON.parse(value('opmlImport') || '[]');
	} catch (e) {
		imported = [];
	}
	if (Array.isArray(imported)) {
		imported.forEach(function(feed) {
			if (feed && typeof feed.url === 'string') {
				add(String(feed.title || feed.url).trim(), feed.url.trim());
			}
		});
	}
	return feeds;
}

module.exports = {
	url: url,
	settingsFromResponse: settingsFromResponse
};
