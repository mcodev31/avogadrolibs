/******************************************************************************

  This source file is part of the Avogadro project.

  Copyright 2013 Kitware, Inc.

  This source code is released under the New BSD License, (the "License").

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

******************************************************************************/

#include "inputgenerator.h"

#include "generichighlighter.h"
#include "quantumpython.h"

#include <avogadro/core/elements.h>
#include <avogadro/core/molecule.h>
#include <avogadro/core/vector.h>

#include <avogadro/io/fileformat.h>
#include <avogadro/io/fileformatmanager.h>

#include <qjsondocument.h>
#include <qjsonarray.h>

#include <QtGui/QBrush>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QScopedPointer>
#include <QtCore/QSettings>
#include <QtCore/QTextStream>

#include <string>

namespace Avogadro {
namespace QtPlugins {

InputGenerator::InputGenerator(const QString &scriptFilePath_)
  : m_debug(!qgetenv("AVO_QM_INPUT_DEBUG").isEmpty()),
    m_moleculeExtension("Unknown"),
    m_scriptFilePath(scriptFilePath_),
    m_displayName(),
    m_options()
{
  m_pythonInterpreter = qgetenv("AVO_PYTHON_INTERPRETER");
  if (m_pythonInterpreter.isEmpty()) {
    m_pythonInterpreter = QSettings().value(
          "quantumInput/interpreters/python").toString();
  }
  if (m_pythonInterpreter.isEmpty())
    m_pythonInterpreter = pythonInterpreterPath;
}

InputGenerator::~InputGenerator()
{
}

QJsonObject InputGenerator::options() const
{
  m_errors.clear();
  if (m_options.isEmpty()) {
    qDeleteAll(m_highlightStyles.values());
    m_highlightStyles.clear();

    // Retrieve/set options
    QByteArray json = execute(QStringList() << "--print-options");

    QJsonDocument doc;
    if (!parseJson(json, doc))
      return m_options;

    if (!doc.isObject()) {
      m_errors << tr("script --print-options output must be an JSON object "
                     "at top level. Received:\n%1").arg(json.constData());
      return m_options;
    }

    m_options = doc.object();

    // Check if the generator needs to read a molecule.
    m_moleculeExtension = "None";
    if (m_options.contains("inputMoleculeFormat") &&
        m_options["inputMoleculeFormat"].isString()) {
      m_moleculeExtension = m_options["inputMoleculeFormat"].toString();
    }

    if (m_options.contains("highlightStyles")
        && m_options.value("highlightStyles").isArray()) {
      if (!parseHighlightStyles(m_options.value("highlightStyles").toArray())) {
        qDebug() << "Failed to parse highlighting styles.";
      }
    }
  }

  return m_options;
}

QString InputGenerator::displayName() const
{
  m_errors.clear();
  if (m_displayName.isEmpty()) {
    m_displayName = QString(execute(QStringList() << "--display-name"));
    m_displayName = m_displayName.trimmed();
  }

  return m_displayName;
}

bool InputGenerator::generateInput(const QJsonObject &options_,
                                   const Core::Molecule &mol)
{
  m_errors.clear();
  m_warnings.clear();
  m_filenames.clear();
  qDeleteAll(m_fileHighlighters.values());
  m_fileHighlighters.clear();
  m_mainFileName.clear();
  m_files.clear();

  // Add the molecule file to the options
  QJsonObject allOptions(options_);
  if (!insertMolecule(allOptions, mol))
    return false;

  QByteArray json(execute(QStringList() << "--generate-input",
                          QJsonDocument(allOptions).toJson()));

  QJsonDocument doc;
  if (!parseJson(json, doc))
    return false;

  // Update cache
  bool result = true;
  if (doc.isObject()) {
    QJsonObject obj = doc.object();

    // Check for any warnings:
    if (obj.contains("warnings")) {
      if (obj["warnings"].isArray()) {
        foreach (const QJsonValue &warning, obj["warnings"].toArray()) {
          if (warning.isString())
            m_warnings << warning.toString();
          else
            m_errors << tr("Non-string warning returned.");
        }
      }
      else {
        m_errors << tr("'warnings' member is not an array.");
      }
    }

    // Extract input file text:
    if (obj.contains("files")) {
      if (obj["files"].isArray()) {
        foreach (const QJsonValue &file, obj["files"].toArray()) {
          if (file.isObject()) {
            QJsonObject fileObj = file.toObject();
            if (fileObj["filename"].isString() &&
                fileObj["contents"].isString()) {
              QString fileName = fileObj["filename"].toString();
              QString contents = fileObj["contents"].toString();
              replaceKeywords(contents, mol);
              m_filenames << fileName;
              m_files.insert(fileObj["filename"].toString(), contents);

              // Concatenate the requested styles for this input file.
              if (fileObj["highlightStyles"].isArray()) {
                GenericHighlighter *highlighter(new GenericHighlighter(this));
                foreach (const QJsonValue &styleVal,
                         fileObj["highlightStyles"].toArray()) {
                  if (styleVal.isString()) {
                    QString styleName(styleVal.toString());
                    if (m_highlightStyles.contains(styleName)) {
                      *highlighter += *m_highlightStyles[styleName];
                    }
                    else {
                      qDebug() << "Cannot find highlight style '"
                               << styleName << "' for file '"
                               << fileName << "'";
                    }
                  }
                }
                if (highlighter->ruleCount() > 0)
                  m_fileHighlighters[fileName] = highlighter;
                else
                  highlighter->deleteLater();
              }
            }
            else {
              result = false;
              m_errors << tr("Malformed file entry: filename/contents missing"
                             " or not strings:\n%1")
                          .arg(QString(QJsonDocument(fileObj).toJson()));
            } // end if/else filename and contents are strings
          }
          else {
            result = false;
            m_errors << tr("Malformed file entry at index %1: Not an object.")
                        .arg(m_filenames.size());
          } // end if/else file is JSON object
        } // end foreach file
      }
      else {
        result = false;
        m_errors << tr("'files' member not an array.");
      } // end if obj["files"] is JSON array
    }
    else {
      result = false;
      m_errors << tr("'files' member missing.");
    } // end if obj contains "files"

    // Extract main input filename:
    if (obj.contains("mainFile")) {
      if (obj["mainFile"].isString()) {
        QString mainFile = obj["mainFile"].toString();
        if (m_filenames.contains(mainFile)) {
          m_mainFileName = mainFile;
        }
        else {
          result = false;
          m_errors << tr("'mainFile' member does not refer to an entry in "
                         "'files'.");
        } // end if/else mainFile is known
      }
      else {
        result = false;
        m_errors << tr("'mainFile' member must be a string.");
      } // end if/else mainFile is string
    }
    else {
      // If no mainFile is specified and there is only one file, use it as the
      // main file. Otherwise, don't set a main input file -- all files will
      // be treated as supplemental input files
      if (m_filenames.size() == 1)
        m_mainFileName = m_filenames.first();
    } // end if/else object contains mainFile
  }
  else {
    result = false;
    m_errors << tr("Response must be a JSON object at top-level.");
  }

  if (result == false)
    m_errors << tr("Script output:\n%1").arg(QString(json));

  return result;
}

int InputGenerator::numberOfInputFiles() const
{
  return m_filenames.size();
}

QStringList InputGenerator::fileNames() const
{
  return m_filenames;
}

QString InputGenerator::mainFileName() const
{
  return m_mainFileName;
}

QString InputGenerator::fileContents(const QString &fileName) const
{
  return m_files.value(fileName, QString());
}

GenericHighlighter *
InputGenerator::createFileHighlighter(const QString &fileName) const
{
  GenericHighlighter *toClone(m_fileHighlighters.value(fileName, NULL));
  return toClone ? new GenericHighlighter(*toClone) : toClone;
}

QByteArray InputGenerator::execute(const QStringList &args,
                                   const QByteArray &scriptStdin) const
{
  QProcess proc;

  // Merge stdout and stderr
  proc.setProcessChannelMode(QProcess::MergedChannels);

  // Add debugging flag if needed.
  QStringList realArgs(args);
  if (m_debug) {
    realArgs.prepend("--debug");
    qDebug() << "Executing" << m_scriptFilePath << realArgs.join(" ")
             << "<" << scriptStdin;
  }

  // Start script
  realArgs.prepend(m_scriptFilePath);
  proc.start(m_pythonInterpreter, realArgs);

  // Write scriptStdin to the process's stdin
  if (!scriptStdin.isNull()) {
    if (!proc.waitForStarted(5000)) {
      m_errors << tr("Error running script '%1 %2': Timed out waiting for "
                     "start (%3).")
                  .arg(m_scriptFilePath, realArgs.join(" "),
                       processErrorString(proc));
      return QByteArray();
    }

    qint64 len = proc.write(scriptStdin);
    if (len != static_cast<qint64>(scriptStdin.size())) {
      m_errors << tr("Error running script '%1 %2': failed to write to stdin "
                     "(len=%3, wrote %4 bytes, QProcess error: %5).")
                  .arg(m_scriptFilePath).arg(realArgs.join(" "))
                  .arg(scriptStdin.size()).arg(len)
                  .arg(processErrorString(proc));
      return QByteArray();
    }
    proc.closeWriteChannel();
  }

  if (!proc.waitForFinished(5000)) {
    m_errors << tr("Error running script '%1 %2': Timed out waiting for "
                   "finish (%3).")
                .arg(m_scriptFilePath, realArgs.join(" "),
                     processErrorString(proc));
    return QByteArray();
  }

  QByteArray result(proc.readAll());

  if (m_debug)
    qDebug() << "Output:" << result;

  return result;
}

bool InputGenerator::parseJson(const QByteArray &json, QJsonDocument &doc) const
{
  QJsonParseError error;
  doc = QJsonDocument::fromJson(json, &error);

  if (error.error != QJsonParseError::NoError) {
    m_errors << tr("Parse error at offset %L1: '%2'\nRaw JSON:\n\n%3")
                .arg(error.offset).arg(error.errorString()).arg(QString(json));
    return false;
  }
  return true;
}

QString InputGenerator::processErrorString(const QProcess &proc) const
{
  QString result;
  switch (proc.error()) {
  case QProcess::FailedToStart:
    result = tr("Script failed to start.");
    break;
  case QProcess::Crashed:
    result = tr("Script crashed.");
    break;
  case QProcess::Timedout:
    result = tr("Script timed out.");
    break;
  case QProcess::ReadError:
    result = tr("Read error.");
    break;
  case QProcess::WriteError:
    result = tr("Write error.");
    break;
  default:
  case QProcess::UnknownError:
    result = tr("Unknown error.");
    break;
  }
  return result;
}

bool InputGenerator::insertMolecule(QJsonObject &json,
                                    const Core::Molecule &mol) const
{
  // Update the cached options if the format is not set
  if (m_moleculeExtension == "Unknown")
    options();

  if (m_moleculeExtension == "None")
    return true;

  Io::FileFormatManager &formats = Io::FileFormatManager::instance();
  QScopedPointer<Io::FileFormat> format(formats.newFormatFromFileExtension(
                                          m_moleculeExtension.toStdString()));

  if (format.isNull()) {
    m_errors << tr("Error writing molecule representation to string: "
                   "Unrecognized file format: %1").arg(m_moleculeExtension);
    return false;
  }

  std::string str;
  if (!format->writeString(str, mol)) {
    m_errors << tr("Error writing molecule representation to string: %1")
                   .arg(QString::fromStdString(format->error()));
    return false;
  }

  if (m_moleculeExtension != "cjson") {
    json.insert(m_moleculeExtension, QJsonValue(QString::fromStdString(str)));
  }
  else {
    // If cjson was requested, embed the actual JSON, rather than the string.
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(str.c_str(), &error);
    if (error.error != QJsonParseError::NoError) {
      m_errors << tr("Error generating cjson object: Parse error at offset %1: "
                     "%2\nRaw JSON:\n\n%3").arg(error.offset)
                  .arg(error.errorString()).arg(QString::fromStdString(str));
      return false;
    }

    if (!doc.isObject()) {
      m_errors << tr("Error generator cjson object: Parsed JSON is not an "
                     "object:\n%1").arg(QString::fromStdString(str));
      return false;
    }

    json.insert(m_moleculeExtension, doc.object());
  }

  return true;
}

QString InputGenerator::generateCoordinateBlock(const QString &spec,
                                                const Core::Molecule &mol) const
{
  // Coordinate blocks:
  // $$coords:<spec>$$ where <spec> is a character string indicating the
  // atom attributes to print:
  // - 'Z': Atomic number
  // - 'S': Element symbol
  // - 'N': Element name
  // - 'x': x coordinate
  // - 'y': y coordinate
  // - 'z': z coordinate
  // - '0': Literal 0
  // - '1': Literal 1
  // - '_': Space character.
  bool needElementSymbol = spec.contains('S');
  bool needElementName = spec.contains('N');
  bool needPosition =
      spec.contains('x') || spec.contains('y') || spec.contains('z');

  // Loop variables
  size_t numAtoms = mol.atomCount();
  Core::Atom atom;
  unsigned char atomicNumber;
  const char *symbol;
  const char *name;
  Vector3 pos3d;
  QString::const_iterator it;
  QString::const_iterator begin = spec.constBegin();
  QString::const_iterator end = spec.constEnd();

  // The replacement string and text stream
  QString replacement;
  QTextStream stream(&replacement);
  stream.setRealNumberNotation(QTextStream::FixedNotation);
  stream.setRealNumberPrecision(6);
  // Field width for real numbers:
  const int realWidth = 11;

  // Generate the replacement block
  for (size_t atom_i = 0; atom_i < numAtoms; ++atom_i) {
    atom = mol.atom(atom_i);
    atomicNumber = atom.atomicNumber();
    if (needElementSymbol)
      symbol = Core::Elements::symbol(atomicNumber);
    if (needElementName)
      name = Core::Elements::name(atomicNumber);
    if (needPosition)
      pos3d = atom.position3d();

    it = begin;
    while (it != end) {
      switch (it->toLatin1()) {
      case '_':
        // Space character. If we are not at the end of the spec, a space will
        // be added by default after the switch clause. If we are at the end,
        // add a space before the newline that will be added.
        if (it + 1 == end) {
          stream.setFieldWidth(1);
          stream << " ";
        }
        break;
      case 'Z':
        stream.setFieldAlignment(QTextStream::AlignLeft);
        stream.setFieldWidth(3);
        stream << static_cast<int>(atomicNumber);
        break;
      case 'S':
        stream.setFieldAlignment(QTextStream::AlignLeft);
        stream.setFieldWidth(3);
        stream << symbol;
        break;
      case 'N':
        stream.setFieldAlignment(QTextStream::AlignLeft);
        stream.setFieldWidth(13); // longest name is currently 13 char
        stream << name;
        break;
      case 'x':
        stream.setFieldAlignment(QTextStream::AlignRight);
        stream.setFieldWidth(realWidth);
        stream << pos3d.x();
        break;
      case 'y':
        stream.setFieldAlignment(QTextStream::AlignRight);
        stream.setFieldWidth(realWidth);
        stream << pos3d.y();
        break;
      case 'z':
        stream.setFieldAlignment(QTextStream::AlignRight);
        stream.setFieldWidth(realWidth);
        stream << pos3d.z();
        break;
      case '0':
        stream.setFieldAlignment(QTextStream::AlignLeft);
        stream.setFieldWidth(1);
        stream << 0;
        break;
      case '1':
        stream.setFieldAlignment(QTextStream::AlignLeft);
        stream.setFieldWidth(1);
        stream << 1;
        break;
      } // end switch

      stream.setFieldWidth(1);
      stream << (++it != end ? " " : "\n");
    } // end while
  } // end for atom

  // Remove the final newline
  replacement.chop(1);
  return replacement;
}

void InputGenerator::replaceKeywords(QString &str,
                                     const Core::Molecule &mol) const
{
  // Simple keywords:
  str.replace("$$atomCount$$", QString::number(mol.atomCount()));
  str.replace("$$bondCount$$", QString::number(mol.bondCount()));

  // Find each coordinate block keyword in the file, then generate and replace
  // it with the appropriate values.
  QRegExp coordParser("\\$\\$coords:([^\\$]*)\\$\\$");
  int ind = 0;
  while ((ind = coordParser.indexIn(str, ind)) != -1) {
    // Extract spec and prepare the replacement
    const QString keyword = coordParser.cap(0);
    const QString spec = coordParser.cap(1);

    // Replace all blocks with this signature
    str.replace(keyword, generateCoordinateBlock(spec, mol));

  } // end for coordinate block
}

bool InputGenerator::parseHighlightStyles(const QJsonArray &json) const
{
  bool result(true);
  foreach (QJsonValue styleVal, json) {
    if (!styleVal.isObject()) {
      qDebug() << "Non-object in highlightStyles array.";
      result = false;
      continue;
    }
    QJsonObject styleObj(styleVal.toObject());

    if (!styleObj.contains("style")) {
      qDebug() << "Style object missing 'style' member.";
      result = false;
      continue;
    }
    if (!styleObj.value("style").isString()) {
      qDebug() << "Style object contains non-string 'style' member.";
      result = false;
      continue;
    }
    QString styleName(styleObj.value("style").toString());

    if (m_highlightStyles.contains(styleName)) {
      qDebug() << "Duplicate highlight style: " << styleName;
      result = false;
      continue;
    }

    if (!styleObj.contains("rules")) {
      qDebug() << "Style object" << styleName << "missing 'rules' member.";
      result = false;
      continue;
    }
    if (!styleObj.value("rules").isArray()) {
      qDebug() << "Style object" << styleName
               << "contains non-array 'rules' member.";
      result = false;
      continue;
    }
    QJsonArray rulesArray(styleObj.value("rules").toArray());

    GenericHighlighter *highlighter(new GenericHighlighter(
                                      const_cast<InputGenerator*>(this)));
    if (!parseRules(rulesArray, *highlighter)) {
      qDebug() << "Error parsing style" << styleName << endl
               << QString(QJsonDocument(styleObj).toJson());
      highlighter->deleteLater();
      result = false;
      continue;
    }
    m_highlightStyles.insert(styleName, highlighter);
  }

  return result;
}

bool InputGenerator::parseRules(const QJsonArray &json,
                                GenericHighlighter &highligher) const
{
  bool result(true);
  foreach (QJsonValue ruleVal, json) {
    if (!ruleVal.isObject()) {
      qDebug() << "Rule is not an object.";
      result = false;
      continue;
    }
    QJsonObject ruleObj(ruleVal.toObject());

    if (!ruleObj.contains("patterns")) {
      qDebug() << "Rule missing 'patterns' array:" << endl
               << QString(QJsonDocument(ruleObj).toJson());
      result = false;
      continue;
    }
    if (!ruleObj.value("patterns").isArray()) {
      qDebug() << "Rule 'patterns' member is not an array:" << endl
               << QString(QJsonDocument(ruleObj).toJson());
      result = false;
      continue;
    }
    QJsonArray patternsArray(ruleObj.value("patterns").toArray());

    if (!ruleObj.contains("format")) {
      qDebug() << "Rule missing 'format' object:" << endl
               << QString(QJsonDocument(ruleObj).toJson());
      result = false;
      continue;
    }
    if (!ruleObj.value("format").isObject()) {
      qDebug() << "Rule 'format' member is not an object:" << endl
               << QString(QJsonDocument(ruleObj).toJson());
      result = false;
      continue;
    }
    QJsonObject formatObj(ruleObj.value("format").toObject());

    GenericHighlighter::Rule &rule = highligher.addRule();

    foreach (QJsonValue patternVal, patternsArray) {
      QRegExp pattern;
      if (!parsePattern(patternVal, pattern)) {
        qDebug() << "Error while parsing pattern:" << endl
            << QString(QJsonDocument(patternVal.toObject()).toJson());
        result = false;
        continue;
      }
      rule.addPattern(pattern);
    }

    QTextCharFormat format;
    if (!parseFormat(formatObj, format)) {
      qDebug() << "Error while parsing format:" << endl
               << QString(QJsonDocument(formatObj).toJson());
      result = false;
    }
    rule.setFormat(format);
  }

  return result;
}

bool InputGenerator::parseFormat(const QJsonObject &json,
                                 QTextCharFormat &format) const
{
  // Check for presets first:
  if (json.contains("preset")) {
    if (!json["preset"].isString()) {
      qDebug() << "Preset is not a string.";
      return false;
    }

    QString preset(json["preset"].toString());
    /// @todo Store presets in a singleton that can be configured in the GUI,
    /// rather than hardcoding them.
    if (preset == "title") {
      format.setFontFamily("serif");
      format.setForeground(Qt::darkGreen);
      format.setFontWeight(QFont::Bold);
    }
    else if (preset == "keyword") {
      format.setFontFamily("mono");
      format.setForeground(Qt::darkBlue);
    }
    else if (preset == "property") {
      format.setFontFamily("mono");
      format.setForeground(Qt::darkRed);
    }
    else if (preset == "literal") {
      format.setFontFamily("mono");
      format.setForeground(Qt::darkMagenta);
    }
    else if (preset == "comment") {
      format.setFontFamily("serif");
      format.setForeground(Qt::darkGreen);
      format.setFontItalic(true);
    }
    else {
      qDebug() << "Invalid style preset: " << preset;
      return false;
    }
    return true;
  }

  // Extract an RGB tuple from 'array' as a QBrush:
  struct {
    QBrush operator()(const QJsonArray &array, bool *ok)
    {
      *ok = false;
      QBrush result;

      if (array.size() != 3)
        return result;

      int rgb[3];
      for (int i = 0; i < 3; ++i) {
        if (!array.at(i).isDouble())
          return result;
        rgb[i] = static_cast<int>(array.at(i).toDouble());
        if (rgb[i] < 0 || rgb[i] > 255) {
          qDebug() << "Warning: Color component value invalid: " << rgb[i]
                   << " (Valid range is 0-255).";
        }
      }

      result.setColor(QColor(rgb[0], rgb[1], rgb[2]));
      result.setStyle(Qt::SolidPattern);
      *ok = true;
      return result;
    }
  } colorParser;

  if (json.contains("foreground")
      && json.value("foreground").isArray()) {
    QJsonArray foregroundArray(json.value("foreground").toArray());
    bool ok;
    format.setForeground(colorParser(foregroundArray, &ok));
    if (!ok)
      return false;
  }

  if (json.contains("background")
      && json.value("background").isArray()) {
    QJsonArray backgroundArray(json.value("background").toArray());
    bool ok;
    format.setBackground(colorParser(backgroundArray, &ok));
    if (!ok)
      return false;
  }

  if (json.contains("attributes")
      && json.value("attributes").isArray()) {
    QJsonArray attributesArray(json.value("attributes").toArray());
    format.setFontWeight(attributesArray.contains(QLatin1String("bold"))
                         ? QFont::Bold : QFont::Normal);
    format.setFontItalic(attributesArray.contains(QLatin1String("italic")));
    format.setFontUnderline(
          attributesArray.contains(QLatin1String("underline")));
  }

  if (json.contains("family")
      && json.value("family").isString()) {
    format.setFontFamily(json.value("family").toString());
  }

  return true;
}

bool InputGenerator::parsePattern(const QJsonValue &json,
                                  QRegExp &pattern) const
{
  if (!json.isObject())
    return false;

  QJsonObject patternObj(json.toObject());

  if (patternObj.contains("regexp")
      && patternObj.value("regexp").isString()) {
    pattern.setPatternSyntax(QRegExp::RegExp2);
    pattern.setPattern(patternObj.value("regexp").toString());
  }
  else if (patternObj.contains("wildcard")
           && patternObj.value("wildcard").isString()) {
    pattern.setPatternSyntax(QRegExp::WildcardUnix);
    pattern.setPattern(patternObj.value("wildcard").toString());
  }
  else if (patternObj.contains("string")
           && patternObj.value("string").isString()) {
    pattern.setPatternSyntax(QRegExp::FixedString);
    pattern.setPattern(patternObj.value("string").toString());
  }
  else {
    return false;
  }

  if (patternObj.contains("caseSensitive")) {
    pattern.setCaseSensitivity(patternObj.value("caseSensitive").toBool(true)
                               ? Qt::CaseSensitive : Qt::CaseInsensitive);
  }

  return true;
}

} // namespace QtPlugins
} // namespace Avogadro
