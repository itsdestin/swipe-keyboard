/* Synthetic, hand-written stand-in for the parts of Steam's UI bundle the contract looks at.
   Not Valve code: only the anchor strings and shapes the checks need. */
const gr="11146165";
function sendKey(e){var k;switch(e.key){case"Left":k=e.lo;break;case"Right":k=e.hi;break;
break;case"ArrowUp":k=e.io;break;case"ArrowDown":k=e.VM;break}SteamClient.Input.ControllerKeyboardSendText(k)}
SteamClient.Input.RegisterForGameKeyboardMessages(this.OnGameKeyboardMessage),
SteamClient.Input.RegisterForUserKeyboardMessages(this.OnModalKeyboardMessage),
SteamClient.Input.RegisterForUserDismissKeyboardMessages(this.OnDismissKeyboardMessage);
const keyAttrs={"data-key":at,"data-key-row":me,"data-key-col":tr,"data-keycode":dr||0};
extended&&(keyAttrs["data-extended-chars"]=1);
const defaults={m_eInputMode:0,m_eLineInputMode:0};
let ca="text";Xt.m_eInputMode!=0&&(ca="password");
const field={type:"password"};
const ctx="SharedJSContext";
SteamClient.Apps?.AddShortcut(a,b,c,d);
x.set_content_type(IBus.InputPurpose.FREE_FORM,IBus.InputHints.NONE);
const origin="https://steamloopback.host";
