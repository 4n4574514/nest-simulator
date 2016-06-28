# -*- coding: utf-8 -*-
#
# helpers.py
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


def cut_it(trenner, text):
    """
    Cut it.

    cut text by trenner
    """
    import re
    if trenner:
        return re.split(trenner, text)
    else:
        return text


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
