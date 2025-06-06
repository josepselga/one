/* ------------------------------------------------------------------------- *
 * Copyright 2002-2025, OpenNebula Project, OpenNebula Systems               *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may   *
 * not use this file except in compliance with the License. You may obtain   *
 * a copy of the License at                                                  *
 *                                                                           *
 * http://www.apache.org/licenses/LICENSE-2.0                                *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * ------------------------------------------------------------------------- */
import ContextIcon from 'iconoir-react/dist/Folder'
import PropTypes from 'prop-types'
import {
  TabType,
  STEP_ID as EXTRA_ID,
} from '@modules/components/Forms/VNetwork/CreateForm/Steps/ExtraConfiguration'
import { FIELDS } from '@modules/components/Forms/VNetwork/CreateForm/Steps/ExtraConfiguration/context/schema'
import CustomAttributes from '@modules/components/Forms/VNetwork/CreateForm/Steps/ExtraConfiguration/context/customAttributes'

import FormWithSchema from '@modules/components/Forms/FormWithSchema'
import { T } from '@ConstantsModule'

const ContextContent = ({ oneConfig, adminGroup }) => (
  <>
    <FormWithSchema
      id={EXTRA_ID}
      cy="context"
      fields={FIELDS(oneConfig, adminGroup)}
    />
    <CustomAttributes />
  </>
)

ContextContent.propTypes = {
  oneConfig: PropTypes.object,
  adminGroup: PropTypes.bool,
}

/** @type {TabType} */
const TAB = {
  id: 'context',
  name: T.Context,
  icon: ContextIcon,
  Content: ContextContent,
  getError: (error) => FIELDS().some(({ name }) => error?.[name]),
}

export default TAB
