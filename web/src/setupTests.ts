import { afterEach } from 'vitest'
import { cleanup } from '@testing-library/react'
import '@testing-library/jest-dom/vitest'

// vitest doesn't auto-run Testing Library's cleanup between tests the way Jest's
// globals do, so each test's rendered tree would otherwise still be in document.body
// when the next test's render() runs, producing duplicate-element query failures.
afterEach(cleanup)
