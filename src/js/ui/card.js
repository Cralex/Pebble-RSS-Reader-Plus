var util2 = require('util2');
var myutil = require('myutil');
var Emitter = require('emitter');
var WindowStack = require('ui/windowstack');
var Propable = require('ui/propable');
var Window = require('ui/window');
var simply = require('ui/simply');

var textProps = [
  'title',
  'subtitle',
  'body',
];

var textColorProps = [
  'titleColor',
  'subtitleColor',
  'bodyColor',
];

var imageProps = [
  'icon',
  'subicon',
  'banner',
];

var actionProps = [
  'up',
  'select',
  'back',
];

var configProps = [
  'style',
  'backgroundColor',
  // false, or { mode: 'page' | 'end', id: Number }: fit the body to the screen (see
  // SimplyPebble.cardFit). The watch emits a 'fit' event with how many bytes it showed.
  'fit',
];

var accessorProps = textProps.concat(textColorProps).concat(imageProps).concat(configProps);
var clearableProps = textProps.concat(imageProps);

var defaults = {
  status: true,
  backgroundColor: 'white',
  // Always sent: the watch's card is shared, so a card without it would keep the last one's.
  fit: false,
};

var Card = function(cardDef) {
  Window.call(this, myutil.shadow(defaults, cardDef || {}));
  this._dynamic = false;
};

Card._codeName = 'card';

util2.inherit(Card, Window);

util2.copy(Emitter.prototype, Card.prototype);

Propable.makeAccessors(accessorProps, Card.prototype);

Card.prototype._prop = function() {
  if (this === WindowStack.top()) {
    simply.impl.card.apply(this, arguments);
  }
};

Card.prototype._clear = function(flags_) {
  var flags = myutil.toFlags(flags_);
  if (flags === true) {
    clearableProps.forEach(Propable.unset.bind(this.state));
  }
  Window.prototype._clear.call(this, flags_);
};

module.exports = Card;
