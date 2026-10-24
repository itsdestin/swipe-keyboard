// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include "typedwordsession.h"
class TypedWordTest:public QObject {
 Q_OBJECT
 static EditorSnapshot e(QString text){int n=text.size();return {text,n,n,true,Qt::ImhNoPredictiveText};}
 static PredictionPolicy p(){return {1,1,1,true,0,true,true};}
private slots:
 void oneCapitalLetterCapitalizesNotShouts(){
  // WHY: a sentence's first tapped letter is a capital ("H"); that asks for "How", not "HOW" (seen on Plasma 6.9).
  // Two or more capitals ("HE") still mean the person is typing in capitals.
  const auto typed=[](const QString &letters){TypedWordSession s;auto before=e("");
   for(auto c:letters){auto after=e(before.text+c);s.edit(before,after,p(),1,QString(c));s.observe(after,p(),1);before=after;}
   s.suggest({"how","he","hello"});return s.candidates();};
  QVERIFY(typed("H").contains("How"));QVERIFY(!typed("H").contains("HOW"));
  QVERIFY(typed("HE").contains("HELLO"));
 }
 void ownedReplacementLiteralAndStalePress(){
  TypedWordSession s;auto before=e("sentinel ");
  for(auto c:QString("hel")){auto after=e(before.text+c);s.edit(before,after,p(),1,QString(c));QVERIFY(s.observe(after,p(),1));before=after;}
  s.suggest({"hello","help","held"});QCOMPARE(s.candidates(),(QStringList{"help","hello","hel"}));
  auto plan=s.take(1,s.selectionToken(),before,p(),1);QCOMPARE(plan.text,QString("hello "));QCOMPARE(plan.replaceFrom,-3);QCOMPARE(plan.replaceLength,3);
  before=e("");s.edit(before,e("h"),p(),1,"h");s.observe(e("h"),p(),1);s.suggest({"hello","help"});auto old=s.selectionToken();
  s.edit(e("h"),e("he"),p(),1,"e");s.observe(e("he"),p(),1);s.suggest({"hello","help"});QVERIFY(s.take(1,old,e("he"),p(),1).isEmpty());
 }
 void heldCompletionIsRevokedByFieldSelectionPurposeAndProfile(){
  for(int kind=0;kind<5;++kind){
   TypedWordSession s;s.edit(e(""),e("h"),p(),1,"h");s.observe(e("h"),p(),1);s.suggest({"hello","help"});auto pressed=s.selectionToken();
   auto changed=e("h");auto policy=p();quint64 epoch=1;
   if(kind==0)changed.anchor=0;
   if(kind==1)changed.hints|=Qt::ImhSensitiveData;
   if(kind==2)policy.completionAllowed=false;
   if(kind==3)++epoch;
   if(kind==4)changed.preedit=true;
   s.changed(changed,policy,epoch);QVERIFY(s.take(1,pressed,e("h"),p(),1).isEmpty());
  }
 }
 void acceptedSpellingEndsWordWithOneSpace(){
  TypedWordSession s;auto before=e("");
  for(auto c:QString("hel")){auto next=e(before.text+c);s.edit(before,next,p(),1,QString(c));s.observe(next,p(),1);before=next;}
  s.suggest({"hello","help"});auto words=s.candidates();const int literal=words.indexOf("hel");QVERIFY(literal>=0);
  // 0.3.1: an accepted spelling (literal too) ends with a space and finishes the word.
  auto plan=s.take(literal,s.selectionToken(),before,p(),1);QCOMPARE(plan.text,QString("hel "));
  QVERIFY(s.candidates().isEmpty());QVERIFY(s.token().isEmpty());
  s.edit(e("hel "),e("hel p"),p(),1,"p");QVERIFY(s.observe(e("hel p"),p(),1));QCOMPARE(s.token(),QString("p"));
 }
 void rejectsPastedPrefixAndBackspacesOnlyOwnedToken(){
  TypedWordSession s;s.edit(e("pasted"),e("pastedh"),p(),1,"h");s.observe(e("pastedh"),p(),1);QVERIFY(s.token().isEmpty());
  s.edit(e(""),e("h"),p(),1,"h");s.observe(e("h"),p(),1);s.edit(e("h"),e("he"),p(),1,"e");s.observe(e("he"),p(),1);
  s.edit(e("he"),e("h"),p(),1,{},true);s.observe(e("h"),p(),1);QCOMPARE(s.token(),QString("h"));
  auto sensitive=e("h");sensitive.hints|=Qt::ImhSensitiveData;s.changed(sensitive,p(),1);QVERIFY(s.token().isEmpty());
 }
};
QTEST_GUILESS_MAIN(TypedWordTest)
#include "tst_typedwordsession.moc"
