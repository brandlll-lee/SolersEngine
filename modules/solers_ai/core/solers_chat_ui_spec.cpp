/**************************************************************************/
/*  solers_chat_ui_spec.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "solers_chat_ui_spec.h"

#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

String SolersChatUISpec::get_chat_pane_rml() {
	return R"RML(<rml>
<head>
	<title>Solers Agent Chat</title>
	<link type="text/rcss" href="solers_chat.rcss"/>
</head>
<body id="solers-chat-root">
	<section class="solers-chat-pane">
		<header class="topbar">
			<button id="panel-toggle" class="control-button icon-only top-icon-button" aria-label="Solers panel">
				<img class="icon-img" src="solers://icon/panel"/>
			</button>
			<span class="topbar-spacer"></span>
			<button id="new-chat-button" class="control-button icon-only top-icon-button" aria-label="New chat">
				<img class="icon-img" src="solers://icon/message-plus"/>
			</button>
			<button id="more-button" class="control-button icon-only top-icon-button" aria-label="More">
				<img class="icon-img" src="solers://icon/more-vertical"/>
			</button>
		</header>
		<main id="messages" class="timeline" data-slot="messages">
			<section class="empty-state">
				<img class="empty-logo" src="solers://logo" alt="Solers"/>
				<div class="empty-copy">
					<h1>What should we build?</h1>
					<p>Describe a world, mechanic, or system.</p>
				</div>
			</section>
		</main>
		<footer id="composer-root" class="composer">
			<div class="composer-input-wrap">
				<textarea id="composer-input" rows="1" placeholder="Ask for follow-up changes"></textarea>
			</div>
			<div class="composer-actions">
				<div class="composer-left">
					<button class="control-button icon-only composer-icon-button" aria-label="Attach context">
						<img class="icon-img" src="solers://icon/plus"/>
					</button>
					<button class="control-button icon-only composer-icon-button access-icon" aria-label="Full access">
						<img class="icon-img" src="solers://icon/shield-orange"/>
					</button>
					<button class="control-button select-chip access-select" aria-label="Access mode">
						<span class="select-label">Full access</span>
						<img class="chevron-img" src="solers://icon/chevron-orange"/>
					</button>
				</div>
				<span class="composer-spacer"></span>
				<div class="composer-right">
					<button class="control-button select-chip model-select" aria-label="Model">
						<span class="model-number">5.5</span>
						<span class="select-label muted-label">Extra High</span>
						<img class="chevron-img" src="solers://icon/chevron"/>
					</button>
					<button id="send-button" class="control-button send-button" aria-label="Send">
						<img class="send-img" src="solers://icon/send-circle"/>
					</button>
				</div>
			</div>
		</footer>
	</section>
</body>
</rml>)RML";
}

String SolersChatUISpec::get_chat_pane_rcss() {
	return R"RCSS(body {
	margin: 0;
	background: #1B1B1D;
	color: #E8EAED;
	font-family: Inter;
	width: 100%;
	height: 100%;
	overflow: hidden;
}

.solers-chat-pane {
	width: 100%;
	height: 100%;
	background: #1B1B1D;
	border-right: 1px #303034;
	display: flex;
	flex-direction: column;
}

.topbar {
	height: 34px;
	display: flex;
	align-items: center;
	gap: 6px;
	padding: 2px 10px 0;
}

button {
	margin: 0;
	border: 0;
	padding: 0;
	background: transparent;
	font-family: Inter;
}

.control-button {
	display: flex;
	align-items: center;
	justify-content: center;
	background: transparent;
	border: 0;
	color: #A3ABBA;
	font-size: 12px;
	line-height: 1;
}

.control-button:hover {
	background: #292A2D;
	color: #D9DEE8;
}

.icon-only {
	width: 30px;
	height: 30px;
	border-radius: 9px;
	padding: 0;
}

.top-icon-button {
	width: 28px;
	height: 28px;
	border-radius: 8px;
}

.icon-img {
	width: 18px;
	height: 18px;
}

.topbar-spacer {
	flex: 1;
}

.timeline {
	padding: 28px 24px 18px;
	display: flex;
	flex-direction: column;
	gap: 20px;
	flex: 1;
	overflow-y: auto;
}

.empty-state {
	flex: 1;
	min-height: 320px;
	display: flex;
	flex-direction: column;
	align-items: center;
	justify-content: center;
	gap: 20px;
	text-align: center;
}

.empty-logo {
	width: 52px;
	height: 52px;
	opacity: 0.9;
}

.empty-copy {
	display: flex;
	flex-direction: column;
	align-items: center;
	gap: 7px;
}

.empty-state h1 {
	margin: 0;
	color: #D6DAE1;
	font-size: 16px;
	font-weight: 600;
}

.empty-state p {
	margin: 0;
	max-width: 260px;
	color: #6C717A;
	font-size: 13px;
	line-height: 1.5;
}

.user-bubble {
	align-self: flex-end;
	background: #0D2B49;
	color: #DCEEFF;
	border-radius: 18px;
	padding: 11px 15px;
	max-width: 270px;
	line-height: 1.45;
}

.meta-row,
.step-kicker {
	color: #858891;
	font-size: 13px;
}

.agent-step-card {
	background: transparent;
	color: #D7D9DD;
	padding: 4px 0;
	line-height: 1.52;
}

.agent-step-card.strong {
	font-weight: 650;
}

.action-row {
	display: flex;
	gap: 7px;
	margin-top: 12px;
}

.action-chip,
.tool-dot {
	background: #242529;
	border: 1px #3A3B40;
	border-radius: 8px;
	color: #AEB3BA;
	padding: 5px 8px;
}

.output-card {
	display: flex;
	align-items: center;
	gap: 12px;
	background: #222326;
	border: 1px #2A2C31;
	border-radius: 14px;
	padding: 14px;
}

.output-icon {
	width: 36px;
	height: 36px;
	border-radius: 10px;
	background: #111C2D;
	border: 1px #0A84FF;
}

.output-copy {
	flex: 1;
	display: flex;
	flex-direction: column;
	gap: 3px;
}

.open-button {
	background: #2A2B30;
	border-radius: 9px;
	padding: 9px 12px;
	color: #F4F6F8;
}

.composer {
	margin: 0 16px 16px;
	border: 1px #313238;
	border-radius: 22px;
	background: #1F2022;
	padding: 12px 12px 8px;
	display: flex;
	flex-direction: column;
	gap: 6px;
}

.composer:hover {
	border: 1px #3C3E45;
}

.composer.focused {
	border: 1px #565E6A;
	background: #212327;
}

.composer-input-wrap {
	flex: 0 0 auto;
	min-height: 26px;
	display: flex;
	padding: 0 4px;
}

textarea {
	width: 100%;
	height: auto;
	min-height: 24px;
	max-height: 184px;
	background: transparent;
	color: #ECEEF2;
	border: 0;
	padding: 2px 2px 0;
	font-size: 14px;
	line-height: 1.5;
	overflow-y: auto;
	caret-color: #E79A5C;
	cursor: text;
}

textarea:focus,
textarea:focus-visible {
	color: #F7F9FC;
}

textarea::placeholder {
	color: #797D86;
}

/* RmlUi ships no default scrollbar styling, so a scrolling element generates a
   zero-width (invisible) scrollbar until it is sized in RCSS. These rules give
   the composer text area and the message timeline a thin, modern, buttonless
   scrollbar instead of clipping overflow with no visible affordance. */
scrollbarvertical {
	width: 10px;
}

scrollbarvertical slidertrack {
	background: transparent;
}

scrollbarvertical sliderbar {
	width: 6px;
	margin-left: 2px;
	margin-right: 2px;
	min-height: 28px;
	border-radius: 4px;
	background: #3C3D44;
}

scrollbarvertical sliderbar:hover {
	background: #4D4F58;
}

scrollbarvertical sliderbar:active {
	background: #5D606B;
}

scrollbarvertical sliderarrowdec,
scrollbarvertical sliderarrowinc {
	width: 0px;
	height: 0px;
}

.composer-actions {
	display: flex;
	align-items: center;
	gap: 8px;
	height: 34px;
	margin-top: 0;
}

.composer-left,
.composer-right {
	display: flex;
	align-items: center;
	gap: 8px;
	height: 34px;
}

.composer-spacer {
	flex: 1;
}

.composer-icon-button {
	color: #9FA8B8;
}

.access-icon {
	color: #FF7D32;
}

.select-chip {
	height: 30px;
	border-radius: 8px;
	background: transparent;
	color: #A9B0BC;
	padding: 0 10px;
	gap: 6px;
	font-size: 12px;
}

.select-chip:hover {
	background: #2A2B2F;
}

.access-select {
	color: #FF7D32;
	min-width: 104px;
	justify-content: flex-start;
}

.model-select {
	min-width: 132px;
	justify-content: flex-end;
}

.select-label {
	line-height: 30px;
}

.model-number {
	color: #F0F3F7;
	font-weight: 700;
	line-height: 30px;
}

.muted-label {
	color: #9DA4AF;
}

.chevron-img {
	width: 12px;
	height: 12px;
}

.send-button {
	width: 34px;
	height: 34px;
	border-radius: 17px;
	background: transparent;
	color: #1A1B1E;
	padding: 0;
	margin-left: 4px;
	flex: 0 0 auto;
}

.send-button:hover {
	background: transparent;
}

.send-img {
	width: 34px;
	height: 34px;
}

.composer .control-button {
	height: 30px;
}

.composer .send-button {
	width: 34px;
	height: 34px;
	border-radius: 17px;
}
)RCSS";
}

Array SolersChatUISpec::get_mock_timeline() {
	return Array();
}
