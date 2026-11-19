/*
 * Minimal RSS 2.0 / RSS 1.0 / Atom parser for PebbleJS.
 *
 * The phone's JavaScript environment has no DOMParser, so this works on the raw text.
 * CDATA sections are swapped out for placeholders first, so markup inside them can't be
 * mistaken for feed elements.
 *
 * parse(xml) returns null if the text isn't a feed, otherwise
 *   { title: String, items: [{ title, author, content }] }
 * where title and author are plain text and content is the article's HTML.
 */

var CDATA_MARK = '\u0000';

function xmlUnescape(text) {
  return text
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&#x([0-9a-f]+);/gi, function(m, hex) { return codePointToString(parseInt(hex, 16)); })
    .replace(/&#(\d+);/g, function(m, dec) { return codePointToString(parseInt(dec, 10)); })
    .replace(/&amp;/g, '&');
}

function codePointToString(code) {
  if (isNaN(code) || code < 0 || code > 0x10FFFF) { return ''; }
  if (code <= 0xFFFF) { return String.fromCharCode(code); }
  code -= 0x10000;
  return String.fromCharCode(0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF));
}

function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function Parser(xml) {
  var cdata = this.cdata = [];
  this.xml = xml
    .replace(/\r\n?/g, '\n') // XML line-ending normalization
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, function(m, content) {
      cdata.push(content);
      return CDATA_MARK + (cdata.length - 1) + CDATA_MARK;
    })
    .replace(/<!--[\s\S]*?-->/g, '');
}

/* Turn element content back into text: unescape XML outside CDATA, keep CDATA verbatim. */
Parser.prototype.text = function(raw, keepMarkup) {
  var cdata = this.cdata;
  var parts = raw.split(CDATA_MARK);
  var out = '';
  for (var i = 0; i < parts.length; i++) {
    if (i % 2 === 1) {
      out += cdata[parseInt(parts[i], 10)] || '';
    } else {
      out += keepMarkup ? parts[i] : xmlUnescape(parts[i]);
    }
  }
  return out.trim();
};

/* All blocks for any of the given element names, as { attrs, inner } */
Parser.prototype.elements = function(source, names) {
  var results = [];
  for (var i = 0; i < names.length; i++) {
    var name = escapeRegExp(names[i]);
    var re = new RegExp('<' + name + '(\\s[^>]*?)?(?:/>|>([\\s\\S]*?)</' + name + '\\s*>)', 'gi');
    var match;
    while ((match = re.exec(source))) {
      results.push({ index: match.index, attrs: match[1] || '', inner: match[2] || '' });
    }
    if (results.length) { break; }
  }
  return results;
};

Parser.prototype.first = function(source, names) {
  return this.elements(source, names)[0];
};

/* Text of the first matching element; Atom type="xhtml" content is already markup. */
Parser.prototype.field = function(source, names) {
  var element = this.first(source, names);
  if (!element) { return ''; }
  var isXhtml = /\btype\s*=\s*["']xhtml["']/i.test(element.attrs);
  return this.text(element.inner, isXhtml);
};

Parser.prototype.author = function(block) {
  var creator = this.field(block, ['dc:creator']);
  if (creator) { return creator; }
  var author = this.first(block, ['author']);
  if (!author) { return ''; }
  var name = this.field(author.inner, ['name']); // Atom: <author><name>…</name></author>
  if (name) { return name; }
  var text = this.text(author.inner);
  // RSS 2.0 <author> is "email (Name)"; show just the name.
  var paren = text.match(/\(([^)]+)\)\s*$/);
  if (paren) { return paren[1]; }
  return text.indexOf('@') === -1 ? text : '';
};

Parser.prototype.parse = function() {
  var xml = this.xml;
  var isAtom = /<feed[\s>]/i.test(xml);
  var isRss = /<(rss|rdf:RDF)[\s>]/i.test(xml);
  if (!isAtom && !isRss) { return null; }

  var blocks = this.elements(xml, isAtom ? ['entry'] : ['item']);
  var self = this;
  var items = blocks.map(function(block) {
    var inner = block.inner;
    return {
      title: self.field(inner, ['title']),
      author: self.author(inner),
      content: self.field(inner, ['content:encoded']) ||
               self.field(inner, ['content']) ||
               self.field(inner, ['description']) ||
               self.field(inner, ['summary'])
    };
  });

  // Feed title: the first <title> before the first item/entry.
  var head = blocks.length ? xml.substring(0, blocks[0].index) : xml;
  return { title: this.field(head, ['title']), items: items };
};

var feedparser = {
  parse: function(xml) {
    if (typeof xml !== 'string') { return null; }
    return new Parser(xml).parse();
  }
};

if (typeof module !== 'undefined') {
  module.exports = feedparser;
}
