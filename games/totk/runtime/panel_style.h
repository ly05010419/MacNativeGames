/* Mac frontend only. The look the settings panel (settings_panel.h) and every sheet it opens share (user 2026-10-26
   "统一设计语言"): a dark ground, a rounded band per row, medium 13 pt titles and grey notes. Included by frontend.m
   before the editors (action_editor.h, controls_editor.h). */
#pragma once
enum { PANEL_W = 600, PANEL_MARGIN = 14, PANEL_ROW_H = 34, PANEL_GAP = 6, PANEL_POP_H = 26 };
static NSView *panel_dark(NSView *view){
 view.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];view.wantsLayer=YES;
 view.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:0.9].CGColor;return view;
}
@interface TOTKPanelRows:NSView
@end
@implementation TOTKPanelRows
-(BOOL)isFlipped{return YES;}
@end
/* In a list (panel_scroll) a band follows the list's width, which a legacy scroller narrows after the rows are laid out. */
static NSView *panel_band(NSView *view,NSRect frame){
 NSView *band=[[NSView alloc] initWithFrame:frame];band.wantsLayer=YES;if([view isKindOfClass:TOTKPanelRows.class])band.autoresizingMask=NSViewWidthSizable;
 band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[view addSubview:band];return band;
}
/* A row's title, vertically centred in a band h high. */
static NSTextField *panel_label(NSView *band,NSString *text,CGFloat x,CGFloat w,CGFloat h){
 NSTextField *label=[NSTextField labelWithString:text];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
 label.textColor=[NSColor colorWithCalibratedWhite:0.92 alpha:1];label.lineBreakMode=NSLineBreakByTruncatingTail;
 label.frame=NSMakeRect(x,floor((h-18)/2),fmax(0,w),18);[band addSubview:label];return label;
}
static NSTextField *panel_note(NSView *view,NSString *text,NSRect frame){
 NSTextField *note=[NSTextField wrappingLabelWithString:text];note.font=[NSFont systemFontOfSize:11];
 note.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];note.frame=frame;[view addSubview:note];return note;
}
static NSPopUpButton *panel_popup(NSView *band,NSArray<NSString *> *names,CGFloat x,CGFloat w,CGFloat h){
 NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(x,floor((h-PANEL_POP_H)/2),w,PANEL_POP_H) pullsDown:NO];
 [p addItemsWithTitles:names];if(band)[band addSubview:p];return p;
}
/* A sheet w x h with the panel's title at its top; its rows start below PANEL_SHEET_TOP. */
enum { PANEL_SHEET_TOP = 50 };
static NSPanel *panel_sheet(NSString *title,CGFloat w,CGFloat h){
 NSPanel *panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,w,h) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
 panel.title=title;panel.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];NSView *view=panel_dark(panel.contentView);
 NSTextField *t=[NSTextField labelWithString:title];t.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 t.textColor=NSColor.whiteColor;t.frame=NSMakeRect(18,h-38,w-36,24);[view addSubview:t];return panel;
}
/* The sheet's bottom line: the left button (wide) and the right ones, the last the default (Return), the one before it Esc. */
static void panel_buttons(NSView *view,CGFloat w,NSArray<NSString *> *titles,id target,const SEL *actions){
 NSUInteger n=titles.count;
 for(NSUInteger i=0;i<n;i++){
  NSButton *b=[NSButton buttonWithTitle:titles[i] target:target action:actions[i]];
  b.frame=i||n==1?NSMakeRect(w-PANEL_MARGIN-(n-i)*100-(n-1-i)*8,12,100,30):NSMakeRect(PANEL_MARGIN,12,150,30);
  if(i==n-1)b.keyEquivalent=@"\r";else if(i==n-2)b.keyEquivalent=@"\e";[view addSubview:b];
 }
}
/* A scrolling list on the sheet's ground: its rows are bands from the top of the returned (flipped) view. */
static NSView *panel_scroll(NSView *view,NSRect frame,CGFloat height,NSScrollView **out){
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:frame];scroll.hasVerticalScroller=YES;scroll.scrollerStyle=NSScrollerStyleOverlay;
 scroll.drawsBackground=NO;scroll.contentView.drawsBackground=NO;scroll.borderType=NSNoBorder;
 NSView *rows=[[TOTKPanelRows alloc] initWithFrame:NSMakeRect(0,0,frame.size.width,height)];rows.autoresizingMask=NSViewWidthSizable;scroll.documentView=rows;
 [view addSubview:scroll];if(out)*out=scroll;return rows;
}
