// require('pebblejs');
var UI = require('ui');
var ajax = require('ajax');
var feedparser = require('feedparser');
var configPage = require('configpage');
var Settings = require('settings');
var Feature = require('platform/feature');
var Vector2 = require('vector2');

// The whole article list is saved to phone storage while reading, so keep it bounded.
var MAX_ARTICLES = 50;

// Article text is split into a first page, which scrolls, and paragraphs. Later pages show as
// much of one paragraph as the watch can fit: the watch measures it (see the card's `fit`
// property) and reports how much it showed, so pages suit each watch's screen and fonts.
// (Declared up here because a saved article is reopened before the rest of this file runs.)
var FIRST_PAGE_MAX_CHARS = 1800; // PebbleJS misbehaves as a page nears ~2,000 characters.
var PAGE_CANDIDATE_MAX_CHARS = 600; // More than any watch can show on one page.
var NBSP = String.fromCharCode(160);
var CONTINUED_MARK = "‹" + NBSP; // Starts a page that continues a paragraph.
var FIRST_PAGE = { unit: -1, offset: 0 };
var END_OF_ARTICLE = null;
var lastFitId = 0;

var blackStatusBar = {
	color: "white",
	backgroundColor: "black",
	separator: 'none'
};

var defaultFeeds = [{
	title: "Vox",
	url: "https://www.vox.com/rss/index.xml"
	},{
	title: "The Atlantic",
	url: "https://www.theatlantic.com/feed/all"
	},{
	title: "Time Magazine",
	url: "https://time.com/feed/"
	},{
	title: "ProPublica",
	url: "https://www.propublica.org/feeds/propublica/main"
	},{
	title: "The Verge",
	url: "https://www.theverge.com/rss/index.xml"
	},{
	title: "Techcrunch",
	url: "https://techcrunch.com/feed/"
	},{
	title: "SBNation",
	url: "https://www.sbnation.com/rss/index.xml"
	}];

// The settings page is generated on the phone (see configpage.js); the url here is unused.
Settings.config({url: 'about:blank', hash: false, autoSave: false}, function(){
	var savedFeeds = Settings.option().feeds;
	return configPage.url({
		feeds: savedFeeds && savedFeeds.length > 0 ? savedFeeds : defaultFeeds,
		wrapFeedList: wrapFeedList(),
		titleScroll: titleScroll()
	});
}, function(e){
	if (!e.failed && e.options) {
		var saved = configPage.settingsFromResponse(e.options);
		Settings.option("feeds", saved.feeds);
		Settings.option("wrapFeedList", saved.wrapFeedList);
		Settings.option("titleScroll", saved.titleScroll);
	}
	feedSelectMenu.hide(); // Will likely close app
});

/* Whether up/down wrap around at the ends of the feed list (a setting; on by default). */
function wrapFeedList() {
	return Settings.option("wrapFeedList") !== false;
}

/* How long titles scroll in menus: 'relaxed' (default), 'quick' or 'off' (a setting). */
function titleScroll() {
	var mode = Settings.option("titleScroll");
	return (mode === 'quick' || mode === 'off') ? mode : 'relaxed';
}

/*-----------------------------------------------------------------------------*/

var articleSelectMenuExists = false;
var feedSelectMenu;
var loadingCardVisible;
var lastSeenArticleCard = {};
var articleSelectMenu = {};

selectFeed();

if (typeof Settings.data('welcomeScreenCompleted') === 'undefined') {
	welcomeScreen();
}
else {
	if (allSavedInfoExists()) {
		displayArticlePage(Settings.data('savedArticleList'), Settings.data('savedArticleNum'), Settings.data('savedPage'), Settings.data('savedPageHistory') || []);
	}
}

function removeSavedInfo() {
	Settings.data('savedArticleList', undefined);
	Settings.data('savedArticleNum', undefined);
	Settings.data('savedPage', undefined);
	Settings.data('savedPageHistory', undefined);
	Settings.data('savedPageNum', undefined); // Saved by older versions
	//savedFeed is NOT removed!
}
function allSavedInfoExists() {
	var list = Settings.data('savedArticleList');
	var num = Settings.data('savedArticleNum');
	var page = Settings.data('savedPage');
	if (typeof Settings.data('savedFeed') === 'undefined' || !Array.isArray(list) || typeof num !== 'number' || !page) {
		return false;
	}
	// Older versions saved articles in a different form; don't try to reopen those.
	var article = list[num];
	return Boolean(article && article.text && Array.isArray(article.text.units) &&
		page.unit < article.text.units.length);
}
function saveCurrPage(articleCard, articleList, articleNum, page, history) {
		Settings.data('savedArticleList', articleList);
		Settings.data('savedArticleNum', articleNum);
		Settings.data('savedPage', page);
		Settings.data('savedPageHistory', history);
		lastSeenArticleCard = articleCard;
}

function selectFeed() {

	/*Overwrite default feeds with what user input in Settings.*/
	if (typeof Settings.option().feeds !== 'undefined' && Settings.option().feeds.length > 0) {
		feeds = Settings.option().feeds;
	}
	else {
		feeds = defaultFeeds;
	}

	feedSelectMenu = new UI.Menu({
		status: false,
		wrap: wrapFeedList(),
		titleScroll: titleScroll(),
		sections: [{
			title: "Select Feed:",
			items: feeds
		}]
	});

	feedSelectMenu.show();

	feedSelectMenu.on('select', function(e) {
		getArticles(feeds[e.itemIndex]);
	});

	feedSelectMenu.on('longSelect', function() {
		welcomeScreen();
	});

	lastSeenArticleCard = {};
	feedSelectMenu.on('show', function(){
		lastSeenArticleCard = {};
	})
}

function getArticles(feed) {
	// A plain window with centered text: cards can't center text on rectangular screens.
	var screen = Feature.resolution();
	loadingCard = new UI.Window({status: blackStatusBar, backgroundColor: 'white'});
	loadingCard.add(new UI.Text({
		position: new Vector2(0, Math.round(screen.y / 2) - 16),
		size: new Vector2(screen.x, 32),
		text: "Loading...",
		font: 'gothic-24-bold',
		color: 'black',
		textAlign: 'center'
	}));
	loadingCard.show();
	loadingCardVisible = true;
	
	Settings.data('savedFeed', feed);
	
	articleList = [];

	function showLoadError(message) {
		var problemCard = new UI.Card({status: blackStatusBar, scrollable: true});
		problemCard.body(message);
		problemCard.show();
		loadingCard.hide();
	}

	ajax({ url: feed.url }, function(body) {
		var parsed;
		try {
			parsed = feedparser.parse(body);
		}
		catch (e) {
			parsed = null;
		}
		if (!parsed) {
			showLoadError("Cannot read this feed. Check that its URL in Settings points to an RSS or Atom feed.");
			return;
		}
		if (parsed.items.length === 0) {
			showLoadError("This feed doesn't have any articles right now.");
			return;
		}
		parsed.items.slice(0, MAX_ARTICLES).forEach(function(item) {
			articleList.push({
				title: formatText(item.title),
				author: formatText(item.author),
				text: makeArticleText(item.content)
			});
		});
		if (loadingCardVisible) {
			selectArticle(articleList, feed.title);
		}
		loadingCard.hide();
	}, function(body, status) {
		showLoadError("Cannot retrieve articles" + (status ? " (error " + status + ")" : "") + ". Check your connection and the feed's URL in Settings.");
	});
	/* Handle case where user returned to menu while article was loading. */
	loadingCard.on('hide', function() {
		loadingCardVisible = false;
	});
}


// Elements that start a new paragraph. (Captions, asides and the like aren't here: formatText
// removes those, so they need to stay in one piece with the paragraph around them.)
// Table cells aren't: each row is one paragraph, with its cells separated by " · ".
var BLOCK_TAGS = "p|h[1-6]|li|ul|ol|dl|dt|dd|div|blockquote|pre|figure|table|thead|tbody|tr|section|article|header|footer|hr";
var BLOCK_OPENING_TAG = new RegExp("<(" + BLOCK_TAGS + ")(\\s[^>]*)?\\/?>", "gi");
var BLOCK_CLOSING_TAG = new RegExp("<\\/(" + BLOCK_TAGS + ")\\s*>", "gi");

function makeArticleText(content) {
	// Break lines around block elements, keeping their tags so formatText still sees whole
	// elements, then split into paragraphs at line breaks. Without this, a heading and the
	// paragraph after it ("<h2>…beta 2</h2><p>Last Monday…") ran together as "beta 2Last Monday".
	var paragraphs = String(content || "")
		.replace(BLOCK_OPENING_TAG, "\n$&")
		.replace(BLOCK_CLOSING_TAG, "$&\n")
		.replace(/<br\b[^>]*>/gi, "\n")
		.replace(/(<\/t[dh]\s*>)\s*(<t[dh]\b)/gi, "$1 · $2")
		.split(/[\r\n\t\f\v]+/);
	paragraphs = paragraphs.map(formatText).filter(Boolean); //remove empty strings

	var first = "";
	var units = [];
	if (paragraphs.length > 0) {
		first = paragraphs[0];
		if (first.length > FIRST_PAGE_MAX_CHARS) {
			var cut = first.lastIndexOf(" ", FIRST_PAGE_MAX_CHARS);
			if (cut <= 0) {
				cut = FIRST_PAGE_MAX_CHARS;
			}
			units.push(CONTINUED_MARK + first.substring(cut).replace(/^ +/, ""));
			first = first.substring(0, cut) + NBSP + "›";
		}
		units = units.concat(paragraphs.slice(1));
	}
	return { first: first, units: units };
}

/* How many characters of text are in its first `bytes` bytes of UTF-8 (as the watch counts). */
function charsInUtf8Prefix(text, bytes) {
	var utf8 = unescape(encodeURIComponent(text));
	try {
		return decodeURIComponent(escape(utf8.substring(0, bytes))).length;
	}
	catch (e) {
		return text.length; // Not at a character boundary; shouldn't happen.
	}
}

/* The page after one that showed `shown` characters of its paragraph from `page.offset`. */
function pageAfter(articleText, page, shown) {
	var text = articleText.units[page.unit];
	var offset = page.offset + shown;
	while (text.charAt(offset) === " ") {
		offset++;
	}
	if (offset < text.length) {
		return { unit: page.unit, offset: offset };
	}
	if (page.unit + 1 < articleText.units.length) {
		return { unit: page.unit + 1, offset: 0 };
	}
	return END_OF_ARTICLE;
}

/* Where "back" goes from a page when there's no history (e.g. after reopening the app). */
function pageBefore(page) {
	if (page.offset > 0) {
		return { unit: page.unit, offset: 0 };
	}
	if (page.unit > 0) {
		return { unit: page.unit - 1, offset: 0 };
	}
	return FIRST_PAGE;
}

function nextFitId() {
	lastFitId = (lastFitId % 65535) + 1; // 1..65535; 0 means "no fit"
	return lastFitId;
}

function formatText(text) {
	if(!text) {
		return "";
	}
	else {
		text = text.replace(/<caption.*?>.*?<\/caption>/gi, "");
		text = text.replace(/<figcaption.*?>.*?<\/figcaption>/gi, "");
		text = text.replace(/<small.*?>.*?<\/small>/gi, "");
		text = text.replace(/<aside.*?>.*?<\/aside>/gi, "");
		text = text.replace(/<cite.*?>.*?<\/cite>/gi, "");

		text = text.replace(/<.*? .*?class=".*?caption.*?".*?>.*?<\/.*?>/gi, "");
		text = text.replace(/<.*? .*?class=".*?credit.*?".*?>.*?<\/.*?>/gi, "");
		text = text.replace(/<.*? .*?data-id="injected-recirculation-link".*?>.*?<\/.*?>/gi, "");
		text = text.replace(/<[^>]*>/gi, ''); //Remove html tags.
		text = text.replace(/[ ]{2,}/gi, ' '); //Remove multiple spaces
		text = text.replace(/&nbsp;/gi, " ");

		text = text.replace(/&quot;/gi, '"');
		text = text.replace(/&[lr]dquo;/gi, '"');
		text = text.replace(/&[lr]squo;/gi, "'");

		text = text.replace(/&ndash;/gi, "–");
		text = text.replace(/&mdash;/gi, "—");
		text = text.replace(/—/gi, String.fromCharCode(8203) + "—" + String.fromCharCode(8203)); //Insert zero width spaces before and after mdash's, to allow line breaks.

		text = text.replace(/&hellip;/gi, "…");

		text = text.replace(/&gt;/gi, '>');
		text = text.replace(/&lt;/gi, '<');

		text = text.replace(/&amp;/gi, "&");
		// Numeric entities, decoded after &amp; so double-escaped ones like &amp;#8217; are fixed too.
		text = text.replace(/&#x([0-9a-f]+);/gi, function(m, hex) { return String.fromCharCode(parseInt(hex, 16)); });
		text = text.replace(/&#(\d+);/g, function(m, dec) { return String.fromCharCode(parseInt(dec, 10)); });
		text = text.replace(/&apos;/gi, "'");

		text = text.replace(/^[<>[\]]*$/g,'');
		text = text.replace(/^Advertisement$/g,'');
		text = text.replace(/^Let's block ads! \(Why\?\)$/g,'');
		text = text.replace(/^Read More[:]{0,1}[ ]{0,1}$/g,'');
		text = text.trim();
		return text;
	}
}

function selectArticle(articleList, heading) {
	/*articleDisplayTitles = [];
	if (! Feature.round()) {
		for (i = 0; i < articleList.length; i++) {
			currTitle = [];
			splitTextCleanly(currTitle, 17, 0, false, articleList[i].title);

			articleDisplayTitles.push({
				title: currTitle[0],
				subtitle: currTitle.slice(1, currTitle.length).join(' ')
			});
		}
	}
	else {
		articleDisplayTitles = articleList;
	}*/

	articleSelectMenu = new UI.Menu({
		status: false,
		titleScroll: titleScroll(),
		sections: [{
			title: heading,
			items: articleList
		}]
	});

	articleSelectMenu.show();
	articleSelectMenuExists = true;
	removeSavedInfo();

	articleSelectMenu.on("select", function(e) {
		displayArticlePage(articleList, e.itemIndex, FIRST_PAGE, []);
	});
	articleSelectMenu.on("longSelect", function() {
		if (Object.keys(lastSeenArticleCard).length !== 0) {
			lastSeenArticleCard.show();
		}
	})
	
	articleSelectMenu.on('show', function() {
		setTimeout(function(){
			//PebbleJS has a tendency to crash here, with certain articles. (The Atlantic is particularly troublesome.)
			//I can't fix the crash because it's not my code, but I can wait a bit before removing savedInfo to make it less painful.
			removeSavedInfo();
		}, 950);
	});
}

/*
 * Show a page of an article: FIRST_PAGE (title, author and the start of the text, scrolling),
 * or { unit, offset }: paragraph `unit` from character `offset`, as much as the watch fits.
 * `history` is the pages before this one, for going back.
 */
function displayArticlePage(articleList, articleNum, page, history) {
	var article = articleList[articleNum];
	var text = article.text;
	var articleCard = new UI.Card({status: blackStatusBar});
	var nextPage; // undefined until known; END_OF_ARTICLE (null) after the last page

	if (page.unit < 0) {
		articleCard.scrollable(true);
		articleCard.title(article.title);
		if (article.author && text.first.indexOf(article.author) === -1) { //If the article has an author which isn't repeated in the first page of the body
			if (article.author.lastIndexOf("by ") === 0) { //If the article's author string starts with "by "
				articleCard.subtitle(article.author);
			}
			else {
				articleCard.subtitle("by " + article.author);
			}
		}
		articleCard.body(text.first);
		nextPage = text.units.length > 0 ? { unit: 0, offset: 0 } : END_OF_ARTICLE;
		articleCard.action({
			up: 'images/action_up.png',
			select: nextPage ? "images/action_next.png" : "images/action_check.png",
			down: 'images/action_down.png'
		});
	}
	else {
		var rest = text.units[page.unit].substring(page.offset);
		var prefix = page.offset > 0 ? CONTINUED_MARK : "";
		var isParagraphEnd = rest.length <= PAGE_CANDIDATE_MAX_CHARS;
		var candidate = rest;
		if (!isParagraphEnd) {
			var cut = rest.lastIndexOf(" ", PAGE_CANDIDATE_MAX_CHARS);
			candidate = rest.substring(0, cut > 0 ? cut : PAGE_CANDIDATE_MAX_CHARS);
		}
		var body = prefix + candidate;
		var fitId = nextFitId();
		articleCard.scrollable(false);
		articleCard.body(body);
		articleCard.action(false);
		articleCard.fit({ mode: isParagraphEnd ? 'end' : 'page', id: fitId });
		articleCard.on('fit', function(e) {
			if (e.id !== fitId) {
				return;
			}
			var shown = charsInUtf8Prefix(body, e.length) - prefix.length;
			if (shown <= 0) {
				shown = candidate.length; // Always make progress.
			}
			nextPage = pageAfter(text, page, shown);
		});
	}

	articleCard.show();

	saveCurrPage(articleCard, articleList, articleNum, page, history);
	articleCard.on('show', function() {
		saveCurrPage(articleCard, articleList, articleNum, page, history);
	});

	function backToArticleList(selectNext) {
		if (articleSelectMenuExists) {
			articleSelectMenu.show();
			articleSelectMenu.selection(0, articleNum + (selectNext ? 1 : 0)); //Note: will leave wrong story selected if feed was updated since article was saved.
		}
		else {
			getArticles(Settings.data('savedFeed'));
		}
	}

	articleCard.on('click', function(e) {
		if (e.button === 'select' || e.button === 'down') {
			if (nextPage === undefined) {
				return; // The watch hasn't said where this page ends yet.
			}
			if (nextPage) {
				displayArticlePage(articleList, articleNum, nextPage, history.concat([page]));
			}
			else {
				backToArticleList(true);
			}
		}
		if (e.button === 'up' || e.button === 'back') {
			if (page.unit >= 0) {
				var previous = history.length > 0 ? history[history.length - 1] : pageBefore(page);
				displayArticlePage(articleList, articleNum, previous, history.slice(0, -1));
			}
			else {
				backToArticleList(false);
			}
		}
		this.hide();
	});
	articleCard.on('longClick', 'select', function() {
		backToArticleList(false);
		this.hide();
	});
	
	//Hack to override back button so PebbleJS lets us handle it above.
	articleCard.on('click', 'back', function(){});
}

/*-----------------------------------------------------------------------------*/

/*There are many cleverer ways I could have written this welcome sequence. However, this way was the easiest.*/

function welcomeScreen() {
	switch(Settings.data('lastSeenWelcomeScreenNum')) {
		default:
			welcomeScreen1();
			break;
		case 2:
			welcomeScreen2();
			break;
		case 3:
			welcomeScreen3();
			break;
		case 4:
			welcomeScreen4();
			break;
		case 5:
			welcomeScreen5();
			break;
		case 6:
			welcomeScreen6();
			break;
		case 7:
			welcomeScreen7();
			break;
	}
}

function welcomeScreen1() {
	Settings.data('lastSeenWelcomeScreenNum', 1);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.scrollable(true);
	welcomeCard.action({
		up: 'images/action_up.png',
		select: "images/action_next.png",
		down: 'images/action_down.png'
	});	

	welcomeCard.body("Welcome to RSS Reader+!\n\nBefore you start reading articles, there's a few things you should know!");
	welcomeCard.show();

	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('click', 'select', function() {
		welcomeScreen2();
		this.hide();
	});
	welcomeCard.on('longClick', function(e) {if (e.button === 'select') {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();}});
}

function welcomeScreen2() {
	Settings.data('lastSeenWelcomeScreenNum', 2);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("Press the BACK or UP buttons to return to the previous page.\n\nPress the SELECT or DOWN buttons to go to the next page.");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen1();this.hide();}if (e.button === 'select' || e.button === 'down') {welcomeScreen3();this.hide();}});
}

function welcomeScreen3(){
	Settings.data('lastSeenWelcomeScreenNum', 3);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("Some paragraphs are too long to fit on a single page. If you see a symbol at the end of a page, it ›");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen2();this.hide();}if (e.button === 'select' || e.button === 'down') {welcomeScreen4();this.hide();}});
}

function welcomeScreen4(){
	Settings.data('lastSeenWelcomeScreenNum', 4);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("‹ means that the current paragraph will be continued on the next page.");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen3();this.hide();}if (e.button === 'select' || e.button === 'down') {welcomeScreen5();this.hide();}});
}

function welcomeScreen5(){
	Settings.data('lastSeenWelcomeScreenNum', 5);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("Hold down the BACK button to exit the app. We'll remember where you left off.");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen4();this.hide();}if (e.button === 'select' || e.button === 'down') {welcomeScreen6();this.hide();}});
}

function welcomeScreen6(){
	Settings.data('lastSeenWelcomeScreenNum', 6);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("Hold down the SELECT button to open the menu. Do it again to return to what you were reading.");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen5();this.hide();}if (e.button === 'select' || e.button === 'down') {welcomeScreen7();this.hide();}});
}

function welcomeScreen7(){
	Settings.data('lastSeenWelcomeScreenNum', 7);
	welcomeCard = new UI.Card({status: blackStatusBar});
	welcomeCard.body("That's all for now! Open the menu to choose a feed.");
	welcomeCard.show();
	welcomeCard.on('click', 'back', function(){}); //Disable back button.
	welcomeCard.on('click', function(e) {if (e.button === 'up' || e.button === 'back') {welcomeScreen6();this.hide();}});
	welcomeCard.on('longClick', function(e) {Settings.data('welcomeScreenCompleted', true);feedSelectMenu.show();this.hide();});
}