#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QTextStream>
#include "profilediagnostics.h"
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Read-only queried profile capability; not active runtime status");
    parser.addHelpOption();
    for(const auto &name:{"model-root","packs-root","locale","layout","mode"})
        parser.addOption(QCommandLineOption(name,QString("Explicit %1 query").arg(name),"value"));
    parser.process(app);
    for(const auto &name:{"model-root","locale","layout","mode"}) if(!parser.isSet(name)) parser.showHelp(2);
    const auto locale=parser.value("locale");
    const auto root=swipekb::canonicalLocale(locale).startsWith("en") || !parser.isSet("packs-root")
        ? parser.value("model-root") : parser.value("packs-root");
    const auto p=swipekb::resolveProfile(locale,parser.value("layout"),parser.value("mode"),root);
    auto d=swipekb::profileDiagnostics(p);
    // WHY installation verification cannot assert an engine was loaded or a field permits prediction.
    d["scope"]="queried-installation-capability";
    d["engineReady"]="unknown";
    d["activeFieldPolicy"]="unknown";
    QTextStream(stdout)<<QJsonDocument::fromVariant(d).toJson(QJsonDocument::Compact)<<'\n';
    return p.capabilities.swipe ? 0 : 1;
}
