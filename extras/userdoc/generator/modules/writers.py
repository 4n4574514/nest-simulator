# -*- coding: utf-8 -*-
#
# writers.py
#
# This file is part of NEST.
#
# Copyright (C) 2004 The NEST Initiative
#
# NEST is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# NEST is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with NEST.  If not, see <http://www.gnu.org/licenses/>.

"""
NEST help writer
================

Collect all the data and write help files.
"""

import os
import re

# from modules.writers import coll_data, check_ifdef

if os.path.isdir('../cmds'):
    pass
else:
    os.mkdir('../cmds')
if os.path.isdir('../cmds/sli'):
    pass
else:
    os.mkdir('../cmds/sli')
if os.path.isdir('../cmds/cc'):
    pass
else:
    os.mkdir('../cmds/cc')


def write_help_html(doc_dic, file, sli_command_list):
    """
    Write html.

    Write html for integration in NEST Help-System
    """
    name = ""
    header_ref = '''<!DOCTYPE html>
    <html>
        <head>
    '''
    header_style = '''
    <style type="text/css">
      body {
        padding: 0;
        margin: 0;
      }
      a {
        color: #339;
        text-decoration: none;
      }
      a:visited {
        color: #339;
        text-decoration: none;
      }
      a:hover {
        text-decoration: underline;
      }
      h1 {
        padding: 15px 0 0 15px;
      }
      p {
        padding-left: 15px;
      }
      table.headerfooter {
        margin: 20px 0 20px 0;
        background-color: #eee;
        width: 100%;
        height: 30px;
        border-top: 2px solid #ccc;
        border-bottom: 2px solid #ccc;
        text-align: center;
      }
      div.wrap {
        padding-left: 15px;
      }
    </style>
    '''

    footer = '''
        </body>
    </html>'''

    linkline = '''
    <table class="headerfooter">
      <tbody><tr>
        <td width="30%" align="center"><a href="../../index.html">
            NEST HelpDesk</a></td>
        <td width="30%" align="center"><a href="../helpindex.html">
            Command Index</a></td>
        <td width="30%" align="center"><a href="../../quickref.html">
            NEST Quick Reference</a></td>
      </tr>
    </tbody></table>
    '''

    copyright = '''
    <p style="text-align:center">
        &copy; 2016 <a href="http://www.nest-initiative.org">
        The NEST Initiative</a>
    </p>
    '''
    htmllist = [header_ref]
    fullname = ""
    # namelist = []
    for key, value in doc_dic.iteritems():
        if key == "Name":
            name = value.strip()
            htmllist.append('<title>NEST Command Index: %s</title></head>'
                            % name)
            htmllist.append(header_style + '<body>')
            htmllist.append('<h1>Command: %s</h1>' % name)
            htmllist.append(linkline + '<div class="wrap">')
    for key, value in doc_dic.iteritems():
        if key == "FullName":
            fullname = value.strip("###### ###### $$")
            fullname = re.sub("(######)", " <br/> ", fullname)
            fullname = re.sub("(\$\$)", '  ', fullname)
            htmllist.append('<b>Name:</b><pre>%s</pre>' % fullname)
    for key, value in doc_dic.iteritems():
        if (key != "Name" and key != "SeeAlso" and key != "Id" and
                key != "File" and key != "FullName"):
            value = re.sub("(######)", " <br/> ", value)
            value = re.sub("(\$\$)", '  ', value)
            htmllist.append('<b>%s: </b>' % key)
            htmllist.append('<pre>%s</pre>' % value)
    for key, value in doc_dic.iteritems():
        if key == "SeeAlso":
            htmllist.append('<b>%s: </b>' % key)
            htmllist.append('<ul>')
            for i in value:
                see = i.strip("###### ###### $$")
                if see:
                    if see in sli_command_list:
                        htmllist.append('    <li><a href="../sli/' + see +
                                        '.html">' + see + '</a></li>')
                    else:
                        htmllist.append('    <li><a href="../cc/' + see +
                                        '.html">' + see + '</a></li>')
            htmllist.append('</ul>')
    for key, value in doc_dic.iteritems():
        if key == "File":
            value = value.strip("###### ###### $$")
            htmllist.append('<b>Source:</b><pre>%s</pre>' % value)
    htmllist.append('</div>' + linkline)
    htmllist.append(copyright)
    htmllist.append(footer)
    if name:  # only, if there is a name
        if file.endswith('.sli'):
            f_file_name = open(('../cmds/sli/%s.html' % name), 'w')
            f_file_name.write('\n'.join(htmllist))
            f_file_name.close()
        else:
            f_file_name = open(('../cmds/cc/%s.html' % name), 'w')
            f_file_name.write('\n'.join(htmllist))
            f_file_name.close()
        # return name


def write_helpindex(index_dic_list):
    """
    Write helpindex.html.

    Collect the long list of dicts and transform it toa sorted html file.
    """
    head = '''
    <!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML//EN">
    <html>
    <head>
      <title>NEST Command Index</title>
      <style type="text/css">
        body {
          padding: 0;
          margin: 0;
        }
        h1 {
          padding: 15px 0 0 15px;
        }
        p {
          padding-left: 15px;
        }
        a {
          color: #339;
          text-decoration: none;
        }
        a:visited {
          color: #339;
          text-decoration: none;
        }
        a:hover {
          text-decoration: underline;
        }
        h1 a {
          color: #000;
          text-decoration: none;
        }
        table.headerfooter {
          margin: 20px 0 20px 0;
          background-color: #eee;
          width: 100%;
          height: 30px;
          border-top: 2px solid #ccc;
          border-bottom: 2px solid #ccc;
          text-align: center;
        }
        table.commands {
          margin: 15px 0 15px 0;
          background-color: #eee;
          width: 90%;
          border: 2px solid #ccc;
          border-spacing: 0px;
          border-collapse: collapse;
        }
        table.commands td {
          border-bottom: 1px solid #ccc;
          border-right: 1px dotted #ccc;
          padding: 5px 0 5px 10px;
          text-align: left;
        }
        table.letteridx {
          margin: 0;
          background-color: #eee;
          width: 90%;
          border: 2px solid #ccc;
          border-spacing: 0px;
          border-collapse: collapse;
        }
        table.letteridx td {
          border-right: 1px solid #ccc;
          padding: 5px;
          text-align: center;
        }
        table.letteridx a {
          display: block;
          height: 100%;
          width: 100%;
        }
        td.left{
            width:30%;
        }
      </style>
    </head>
    <body bgcolor="white" fgcolor="black">
    <h1><a name="top">NEST Command Index</a></h1>
    <table class="headerfooter">
      <tr>
        <td width="50%" align=center>
        <a href="../index.html">NEST HelpDesk</a></td>
        <td width="50%" align=center>
        <a href="../quickref.html">NEST Quick Reference</a></td>
      </tr>
    </table>
    '''

    footer = '''
    <table class="headerfooter">
        <tr>
        <td width="30%" align=center>
        <a href="../index.html">NEST HelpDesk</a></td>
        <td width="30%" align=center><a href="#top">Top</a></td>
        <td width="30%" align=center>
        <a href="../quickref.html">NEST Quick Reference</a></td>
        </tr>
    </table>

    <p style="text-align:center">
    &copy; 2016 <a href="http://www.nest-initiative.org">The NEST Initiative
    </a>
    </p>
    </body>
    </html>
    '''
    alpha = [('A', 'a'), ('B', 'b'), ('C', 'c'), ('D', 'd'), ('E', 'e'),
             ('F', 'f'), ('G', 'g'), ('H', 'h'), ('I', 'i'), ('J' 'j'),
             ('K', 'k'), ('L', 'l'), ('M', 'm'), ('N', 'n'), ('O', 'o'),
             ('P', 'p'), ('Q', 'q'), ('R', 'r'), ('S', 's'), ('T', 't'),
             ('U', 'u'), ('V', 'v'), ('W', 'w'), ('X', 'x'), ('Z', 'z'), '-',
             ':', '<', '=']

    html_list = [head]

    from operator import itemgetter
    index_dic_list = sorted(index_dic_list, key=itemgetter('name'))
    # print index_dic_list
    for doubles in alpha:
        # print '#############################\n' + doubles[0]
        html_list.append('<center><table>')
        html_list.append('<a name="%s">' % doubles[0])
        html_list.append('<table class="letteridx"><tr>')
        for x in alpha:
            html_list.append('<td><a href="#%s">%s</a></td>' % (x[0], x[0]))
        html_list.append('</table></center>')
        html_list.append('<center><table class="commands">')
        for item in index_dic_list:
            if item['name'].startswith(doubles):
                # ERROR?
                # if item['name'] == "tsodyks_facilitating":
                #     continue
                html_list.append('<tr><td class="left">')
                html_list.append('<a href="%s/%s.html">%s</a></td>' %
                                 (item['ext'], item['name'], item['name']))
                html_list.append('<td>%s</td></tr>' % item['fullname'])
        html_list.append('</table></center>')
    html_list.append(footer)
    f_helpindex = open('../cmds/helpindex.html', 'w')
    f_helpindex.write('\n'.join(html_list))
    f_helpindex.close()


def coll_data(keywords, documentation, num, file, sli_command_list):
    """
    Collect data.

    Prepare the data for writing the help.
    """
    from helpers import cut_it
    iname = ""
    see = ""
    ifullname = ""
    relfile = file.strip()
    doc_dic = {"Id": str(num), "File": relfile}
    for k in keywords:
        if k in documentation:
            if k == "Name:":
                iname = cut_it("-", documentation[k])
                iname = iname[0].strip(" ######")
                ifullname = documentation[k].strip(" ######")
                if iname:
                    doc_dic.update({"Name": iname})
                if ifullname:
                    doc_dic.update({"FullName": ifullname})
            elif k == "SeeAlso:" or k == "See also:" or k == "See Also:":
                doc_list = []
                see_alsos = cut_it(",", documentation[k])
                for i in see_alsos:
                    see = i.strip()
                    if see:
                        doc_list.append(see)
                doc_dic.update({"SeeAlso": doc_list})
            else:
                text = ""
                name = k.replace(":", "")
                for i in cut_it("\n", documentation[k]):
                    text = text + i.strip() + " \n" + ""
                if text:
                    doc_dic.update({name: text})

    # all_list.append(doc_dic)
    write_help_html(doc_dic, file, sli_command_list)
    # return(write_help_md(doc_dic))


def check_ifdef(item, filetext, docstring):
        """
        Check the ifdef context.

        If there is an ifdef requirement write it to the data.
        """
        import re
        ifdefstring = r'(\#ifdef((.*?)\n(.*?)\n*))\#endif'
        require_reg = re.compile('HAVE\_((.*?)*)\n')
        # every doc in an #ifdef
        ifdefs = re.findall(ifdefstring, filetext, re.DOTALL)
        for ifitem in ifdefs:
            for str_ifdef in ifitem:
                initems = re.findall(docstring, str_ifdef, re.DOTALL)
                for initem in initems:
                    if item == initem:
                        features = require_reg.search(str_ifdef)
                        return features.group()
