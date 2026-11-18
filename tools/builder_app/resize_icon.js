// make_app.py icon(): one size of the app's icon, with the system's AppKit (osascript -l JavaScript; nothing to install).
// argv: source image, pixels, output PNG. Drawn into a bitmap of that size and written straight to the output, not
// atomically: sips (and an atomic write) goes through the user's temporary folder on the system disk, which this Mac's
// build sandbox refuses (2026-11-15: "sips … Cannot write to file /var/folders/…/T/…").
ObjC.import('AppKit');

function run(argv) {
  var image = $.NSImage.alloc.initWithContentsOfFile(argv[0]);
  if (image.isNil()) throw new Error('cannot read ' + argv[0]);
  var n = parseInt(argv[1], 10);
  var rep = $.NSBitmapImageRep.alloc.initWithBitmapDataPlanesPixelsWidePixelsHighBitsPerSampleSamplesPerPixelHasAlphaIsPlanarColorSpaceNameBytesPerRowBitsPerPixel(
    null, n, n, 8, 4, true, false, $.NSDeviceRGBColorSpace, 0, 0);
  rep.size = $.NSMakeSize(n, n);
  var context = $.NSGraphicsContext.graphicsContextWithBitmapImageRep(rep);
  $.NSGraphicsContext.saveGraphicsState;   // JXA: a method without arguments is called by reading it
  $.NSGraphicsContext.setCurrentContext(context);
  context.setImageInterpolation($.NSImageInterpolationHigh);
  image.drawInRectFromRectOperationFraction($.NSMakeRect(0, 0, n, n), $.NSZeroRect, $.NSCompositingOperationCopy, 1.0);
  context.flushGraphics;
  $.NSGraphicsContext.restoreGraphicsState;
  var data = rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $());
  if (!data.writeToFileAtomically(argv[2], false)) throw new Error('cannot write ' + argv[2]);
  return '';
}
